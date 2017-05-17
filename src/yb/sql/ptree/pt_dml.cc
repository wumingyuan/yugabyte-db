//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Treenode implementation for DML including SELECT statements.
//--------------------------------------------------------------------------------------------------

#include "yb/sql/ptree/sem_context.h"
#include "yb/sql/ptree/pt_dml.h"

#include "yb/client/schema-internal.h"
#include "yb/common/table_properties_constants.h"

namespace yb {
namespace sql {

using client::YBSchema;
using client::YBTable;
using client::YBTableType;
using client::YBTableName;
using client::YBColumnSchema;

PTDmlStmt::PTDmlStmt(MemoryContext *memctx,
                     YBLocation::SharedPtr loc,
                     bool write_only,
                     PTConstInt::SharedPtr ttl_seconds)
  : PTCollection(memctx, loc),
    is_system_(false),
    table_columns_(memctx),
    num_key_columns_(0),
    num_hash_key_columns_(0),
    key_where_ops_(memctx),
    where_ops_(memctx),
    write_only_(write_only),
    ttl_seconds_(ttl_seconds),
    bind_variables_(memctx),
    column_args_(nullptr) {
}

PTDmlStmt::~PTDmlStmt() {
}

CHECKED_STATUS PTDmlStmt::LookupTable(SemContext *sem_context) {
  YBTableName name = table_name();

  if (!name.has_namespace()) {
    name.set_namespace_name(sem_context->CurrentKeyspace());
  }

  is_system_ = name.is_system();
  if (is_system_ && write_only_ && client::FLAGS_yb_system_namespace_readonly) {
    return sem_context->Error(table_loc(), ErrorCode::SYSTEM_NAMESPACE_READONLY);
  }

  VLOG(3) << "Loading table descriptor for " << name.ToString();
  table_ = sem_context->GetTableDesc(name);
  if (table_ == nullptr) {
    return sem_context->Error(table_loc(), ErrorCode::TABLE_NOT_FOUND);
  }

  const YBSchema& schema = table_->schema();
  const int num_columns = schema.num_columns();
  num_key_columns_ = schema.num_key_columns();
  num_hash_key_columns_ = schema.num_hash_key_columns();

  table_columns_.resize(num_columns);
  for (int idx = 0; idx < num_columns; idx++) {
    // Find the column descriptor.
    const YBColumnSchema col = schema.Column(idx);
    table_columns_[idx].Init(idx,
                             schema.ColumnId(idx),
                             idx < num_hash_key_columns_,
                             idx < num_key_columns_,
                             col.is_static(),
                             col.type(),
                             YBColumnSchema::ToInternalDataType(col.type()));

    // Insert the column descriptor to symbol table.
    MCString col_name(sem_context->PTreeMem(), col.name().c_str(), col.name().size());
    RETURN_NOT_OK(sem_context->MapSymbol(col_name, &table_columns_[idx]));
  }

  return Status::OK();
}

// Node semantics analysis.
CHECKED_STATUS PTDmlStmt::Analyze(SemContext *sem_context) {
  MemoryContext *psem_mem = sem_context->PSemMem();
  column_args_ = MCMakeShared<MCVector<ColumnArg>>(psem_mem);
  return Status::OK();
}

CHECKED_STATUS PTDmlStmt::AnalyzeWhereClause(SemContext *sem_context,
                                             const PTExpr::SharedPtr& where_clause) {
  if (where_clause == nullptr) {
    if (write_only_) {
      return sem_context->Error(loc(), "Missing partition key",
                                ErrorCode::CQL_STATEMENT_INVALID);
    }
    return Status::OK();
  }

  // Analyze where expression.
  if (write_only_) {
    key_where_ops_.resize(num_key_columns_);
  } else {
    key_where_ops_.resize(num_hash_key_columns_);
  }
  RETURN_NOT_OK(AnalyzeWhereExpr(sem_context, where_clause.get()));
  return Status::OK();
}

CHECKED_STATUS PTDmlStmt::AnalyzeWhereExpr(SemContext *sem_context, PTExpr *expr) {
  // Construct the state variables and analyze the expression.
  MCVector<ColumnOpCounter> op_counters(sem_context->PTempMem());
  op_counters.resize(num_columns());
  WhereExprState where_state(&where_ops_, &key_where_ops_, &op_counters, write_only_);

  SemState sem_state(sem_context, DataType::BOOL, InternalType::kBoolValue);
  sem_state.SetWhereState(&where_state);
  RETURN_NOT_OK(expr->Analyze(sem_context));

  if (write_only_) {
    // Make sure that all hash entries are referenced in where expression.
    for (int idx = 0; idx < num_hash_key_columns_; idx++) {
      if (op_counters[idx].eq_count() == 0) {
        return sem_context->Error(expr->loc(),
                                  "Missing condition on key columns in WHERE clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      }
    }

    // If writing static columns only, check that either all range key entries are referenced in the
    // where expression or none is referenced. Else, check that all range key are referenced.
    int range_keys = 0;
    for (int idx = num_hash_key_columns_; idx < num_key_columns_; idx++) {
      if (op_counters[idx].eq_count() != 0) {
        range_keys++;
      }
    }
    if (StaticColumnArgsOnly()) {
      if (range_keys != num_key_columns_ - num_hash_key_columns_ && range_keys != 0)
        return sem_context->Error(expr->loc(),
                                  "Missing condition on key columns in WHERE clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      if (range_keys == 0) {
        key_where_ops_.resize(num_hash_key_columns_);
      }
    } else {
      if (range_keys != num_key_columns_ - num_hash_key_columns_)
        return sem_context->Error(expr->loc(),
                                  "Missing condition on key columns in WHERE clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
    }
  } else {
    // Add the hash to the where clause if the list is incomplete. Clear key_where_ops_ to do
    // whole-table scan.
    bool has_incomplete_hash = false;
    for (int idx = 0; idx < num_hash_key_columns_; idx++) {
      if (!key_where_ops_[idx].IsInitialized()) {
        has_incomplete_hash = true;
        break;
      }
    }
    if (has_incomplete_hash) {
      for (int idx = num_hash_key_columns_ - 1; idx >= 0; idx--) {
        if (key_where_ops_[idx].IsInitialized()) {
          where_ops_.push_front(key_where_ops_[idx]);
        }
      }
      key_where_ops_.clear();
    }
  }

  return Status::OK();
}

CHECKED_STATUS PTDmlStmt::AnalyzeIfClause(SemContext *sem_context,
                                          const PTExpr::SharedPtr& if_clause) {
  if (if_clause != nullptr) {
    SemState sem_state(sem_context, DataType::BOOL, InternalType::kBoolValue);
    return if_clause->Analyze(sem_context);
  }
  return Status::OK();
}

CHECKED_STATUS PTDmlStmt::AnalyzeUsingClause(SemContext *sem_context) {
  if (ttl_seconds_ == nullptr) {
    return Status::OK();
  }

  if (!yb::common::IsValidTTLSeconds(ttl_seconds_->Eval())) {
    return sem_context->Error(ttl_seconds_->loc(),
                              strings::Substitute("Valid ttl range : [$0, $1]",
                                                  yb::common::kMinTtlSeconds,
                                                  yb::common::kMaxTtlSeconds).c_str(),
                              ErrorCode::INVALID_ARGUMENTS);
  }
  return Status::OK();
}

void PTDmlStmt::Reset() {
  for (auto itr = bind_variables_.cbegin(); itr != bind_variables_.cend(); itr++) {
    (*itr)->Reset();
  }
  column_args_ = nullptr;
}

// Are we writing to static columns only, i.e. no range columns or non-static columns.
bool PTDmlStmt::StaticColumnArgsOnly() const {
  if (column_args_->empty()) {
    return false;
  }
  bool write_range_columns = false;
  for (int idx = num_hash_key_columns_; idx < num_key_columns_; idx++) {
    if (column_args_->at(idx).IsInitialized()) {
      write_range_columns = true;
      break;
    }
  }
  bool write_static_columns = false;
  bool write_non_static_columns = false;
  for (int idx = num_key_columns_; idx < column_args_->size(); idx++) {
    if (column_args_->at(idx).IsInitialized()) {
      if (column_args_->at(idx).desc()->is_static()) {
        write_static_columns = true;
      } else {
        write_non_static_columns = true;
      }
      if (write_static_columns && write_non_static_columns) {
        break;
      }
    }
  }
  return write_static_columns && !write_range_columns && !write_non_static_columns;
}

//--------------------------------------------------------------------------------------------------

CHECKED_STATUS WhereExprState::AnalyzeColumnOp(SemContext *sem_context,
                                               const PTRelationExpr *expr,
                                               const ColumnDesc *col_desc,
                                               PTExpr::SharedPtr value) {
  ColumnOpCounter& counter = op_counters_->at(col_desc->index());
  switch (expr->yql_op()) {
    case YQL_OP_EQUAL: {
      if (counter.eq_count() > 0 || counter.gt_count() > 0 || counter.lt_count() > 0) {
        return sem_context->Error(expr->loc(), "Illogical condition for where clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      }
      counter.increase_eq();

      // Check if the column is used correctly.
      if (col_desc->is_hash()) {
        (*key_ops_)[col_desc->index()].Init(col_desc, value, YQLOperator::YQL_OP_EQUAL);
      } else if (col_desc->is_primary()) {
        if (write_only_) {
          (*key_ops_)[col_desc->index()].Init(col_desc, value, YQLOperator::YQL_OP_EQUAL);
        } else {
          ColumnOp col_op(col_desc, value, YQLOperator::YQL_OP_EQUAL);
          ops_->push_back(col_op);
        }
      } else {
        return sem_context->Error(expr->loc(), "Non primary key cannot be used in where clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      }
      break;
    }

    case YQL_OP_LESS_THAN: FALLTHROUGH_INTENDED;
    case YQL_OP_LESS_THAN_EQUAL: {
      if (col_desc->is_hash()) {
        return sem_context->Error(expr->loc(), "Partition column cannot be used in this expression",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      } else if (col_desc->is_primary()) {
        if (write_only_) {
          return sem_context->Error(expr->loc(), "Range expression is not yet supported",
                                    ErrorCode::FEATURE_NOT_YET_IMPLEMENTED);
        } else if (counter.eq_count() > 0 || counter.lt_count() > 0) {
          return sem_context->Error(expr->loc(), "Illogical range condition",
                                    ErrorCode::CQL_STATEMENT_INVALID);
        }
      } else {
        return sem_context->Error(expr->loc(), "Non primary key cannot be used in where clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      }
      counter.increase_lt();

      // Cache the column operator for execution.
      ColumnOp col_op(col_desc, value, expr->yql_op());
      ops_->push_back(col_op);
      break;
    }

    case YQL_OP_GREATER_THAN_EQUAL: FALLTHROUGH_INTENDED;
    case YQL_OP_GREATER_THAN: {
      if (col_desc->is_hash()) {
        return sem_context->Error(expr->loc(), "Partition column cannot be used in this expression",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      } else if (col_desc->is_primary()) {
        if (write_only_) {
          return sem_context->Error(expr->loc(), "Range expression is not yet supported",
                                    ErrorCode::FEATURE_NOT_YET_IMPLEMENTED);
        } else if (counter.eq_count() > 0 || counter.gt_count() > 0) {
          return sem_context->Error(expr->loc(), "Illogical range condition",
                                    ErrorCode::CQL_STATEMENT_INVALID);
        }
      } else {
        return sem_context->Error(expr->loc(), "Non primary key cannot be used in where clause",
                                  ErrorCode::CQL_STATEMENT_INVALID);
      }
      counter.increase_gt();

      // Cache the column operator for execution.
      ColumnOp col_op(col_desc, value, expr->yql_op());
      ops_->push_back(col_op);
      break;
    }

    default:
      return sem_context->Error(expr->loc(), "Operator is not supported in where clause",
                                ErrorCode::CQL_STATEMENT_INVALID);
  }

  return Status::OK();
}

} // namespace sql
} // namespace yb

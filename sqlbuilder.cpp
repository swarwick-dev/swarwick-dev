#include "SqlQueryBuilder.h"
#include <sstream>

SqlQueryBuilder& SqlQueryBuilder::SetTable(const std::string& table) {
    table_ = table;
    return *this;
}

SqlQueryBuilder& SqlQueryBuilder::AddJoin(const std::string& joinClause) {
    joins_.push_back(joinClause);
    return *this;
}

SqlQueryBuilder& SqlQueryBuilder::AddFilter(const std::string& condition) {
    filters_.push_back(condition);
    return *this;
}

SqlQueryBuilder& SqlQueryBuilder::AddColumn(const std::string& column) {
    columns_.push_back(column);
    return *this;
}

SqlQueryBuilder& SqlQueryBuilder::AddCTE(const std::string& cteName, const std::string& cteQuery) {
    ctes_.emplace_back(cteName, cteQuery);
    return *this;
}

SqlQueryBuilder& SqlQueryBuilder::BindParam(const std::string& name, const std::string& value) {
    parameters_[name] = value;
    return *this;
}

std::unordered_map<std::string, std::string> SqlQueryBuilder::GetParameters() const {
    return parameters_;
}

std::string SqlQueryBuilder::Build() const {
    std::ostringstream oss;

    // CTEs
    if (!ctes_.empty()) {
        oss << "WITH ";
        for (size_t i = 0; i < ctes_.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << ctes_[i].first << " AS (" << ctes_[i].second << ")";
        }
        oss << " ";
    }

    // SELECT clause
    oss << "SELECT ";
    if (!columns_.empty()) {
        for (size_t i = 0; i < columns_.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << columns_[i];
        }
    } else {
        oss << "*";
    }

    // FROM clause
    oss << " FROM " << table_;

    // JOIN clauses
    for (const auto& join : joins_) {
        oss << " " << join;
    }

    // WHERE clause
    if (!filters_.empty()) {
        oss << " WHERE ";
        for (size_t i = 0; i < filters_.size(); ++i) {
            if (i > 0) oss << " AND ";
            oss << filters_[i];
        }
    }

    return oss.str();
}
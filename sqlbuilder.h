#pragma once

#include <string>
#include <vector>
#include <unordered_map>

class SqlQueryBuilder {
public:
    SqlQueryBuilder& SetTable(const std::string& table);
    SqlQueryBuilder& AddJoin(const std::string& joinClause);
    SqlQueryBuilder& AddFilter(const std::string& condition);
    SqlQueryBuilder& AddColumn(const std::string& column);
    SqlQueryBuilder& AddCTE(const std::string& cteName, const std::string& cteQuery);
    SqlQueryBuilder& BindParam(const std::string& name, const std::string& value);

    std::string Build() const;
    std::unordered_map<std::string, std::string> GetParameters() const;

private:
    std::string table_;
    std::vector<std::string> joins_;
    std::vector<std::string> filters_;
    std::vector<std::string> columns_;
    std::vector<std::pair<std::string, std::string>> ctes_;
    std::unordered_map<std::string, std::string> parameters_;
};
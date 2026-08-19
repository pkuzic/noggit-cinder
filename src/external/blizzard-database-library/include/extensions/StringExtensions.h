#pragma once
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <locale>

namespace BlizzardDatabaseLib {
    namespace Extension {
    
        class String
        {
        public:
            static std::vector<std::string> Split(const std::string& s, std::string delimiter)
            {
                size_t pos_start = 0, pos_end, delim_len = delimiter.length();
                std::string token;
                std::vector<std::string> res;

                while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
                    token = s.substr(pos_start, pos_end - pos_start);
                    pos_start = pos_end + delim_len;

                    if (token.empty())
                        continue;

                    res.push_back(token);
                }

                res.push_back(s.substr(pos_start));
                return res;
            }

            static bool Compare(const std::string& lhs, const std::string& rhs)
            {
                auto result = strcmp(lhs.c_str(), rhs.c_str());

                if (result == 0)
                    return true;
                return false;
            }

            static bool IgnoreCaseCompare(const std::string& lhs, const std::string& rhs)
            {
                std::locale loc;
                auto lhs_lower = lhs;
                for (std::string::size_type i = 0; i < lhs.length(); ++i)
                    lhs_lower[i] = std::tolower(lhs[i], loc);

                auto rhs_lower = rhs;
                for (std::string::size_type i = 0; i < rhs.length(); ++i)
                    rhs_lower[i] = std::tolower(rhs[i], loc);

                auto result = strcmp(lhs_lower.c_str(), rhs_lower.c_str());

                if (result == 0)
                    return true;
                return false;
            }

            static void Replace(std::string& data, std::string toSearch, std::string replaceStr)
            {
                size_t pos = data.find(toSearch);
                while (pos != std::string::npos)
                {
                    data.replace(pos, toSearch.size(), replaceStr);
                    pos = data.find(toSearch, pos + replaceStr.size());
                }
            }
        };
    }
}

namespace std {
    template <typename _CharT, typename _Traits>
    inline basic_ostream<_CharT, _Traits>& tab(basic_ostream<_CharT, _Traits>& __os) {
        return __os.put(__os.widen('\t'));
    }
}
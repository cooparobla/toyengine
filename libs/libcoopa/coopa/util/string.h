/**
 * @file string.h
 * @brief Utility functions for string manipulations within the StringUtil namespace.
 */

#ifndef COOPA_UTIL_STRING_H
#define COOPA_UTIL_STRING_H

#include <string>
#include <vector>
#include <algorithm>
#include <numeric> // For std::accumulate
#include <cctype>  // For std::tolower and std::toupper

/**
 * @namespace StringUtil
 * @brief Namespace containing simple inline string utility functions.
 */
namespace StringUtil {

    /**
     * @brief Splits a string by a given delimiter.
     * @param str The string to split.
     * @param delimiter The delimiter substring to split on.
     * @return A vector of substring tokens.
     */
    inline std::vector<std::string> split(const std::string& str,
                                            const std::string& delimiter) {
        std::vector<std::string> tokens;
        size_t pos = 0;
        size_t lastPos = 0;
        while ((pos = str.find(delimiter, lastPos)) != std::string::npos) {
            tokens.push_back(str.substr(lastPos, pos - lastPos));
            lastPos = pos + delimiter.length();
        }
        tokens.push_back(str.substr(lastPos));
        return tokens;
    }

    /**
     * @brief Replaces all occurrences of a substring within a string.
     * @param str The source string.
     * @param oldSubstr Substring to find.
     * @param newSubstr Substring to replace it with.
     * @return The modified string.
     */
    inline std::string replace(const std::string& str,
                                const std::string& oldSubstr,
                                const std::string& newSubstr) {
        std::string result = str;
        size_t pos = 0;
        while ((pos = result.find(oldSubstr, pos)) != std::string::npos) {
            result.replace(pos, oldSubstr.length(), newSubstr);
            pos += newSubstr.length();
        }
        return result;
    }

    /**
     * @brief Joins a vector of strings together with a delimiter.
     * @param strings Substrings to join.
     * @param delimiter Delimiter string to place between elements.
     * @return The joined string.
     */
    inline std::string join(const std::vector<std::string>& strings,
                            const std::string& delimiter) {
        if (strings.empty()) {
            return "";
        }
        return std::accumulate(strings.begin() + 1, strings.end(), strings[0],
                                [&](std::string a, std::string b) {
                                    return a + delimiter + b;
                                });
    }

    /**
     * @brief Converts a copy of the string to lowercase.
     * @param str The source string.
     * @return The lowercase string.
     */
    inline std::string lower(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(),
                        [](unsigned char c){ return std::tolower(c); });
        return str;
    }

    /**
     * @brief Converts a copy of the string to uppercase.
     * @param str The source string.
     * @return The uppercase string.
     */
    inline std::string upper(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(),
                        [](unsigned char c){ return std::toupper(c); });
        return str;
    }

} 

#endif // COOPA_UTIL_STRING_H
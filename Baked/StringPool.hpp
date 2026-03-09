#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fce
{
    /**
     * String intern pool — each unique string is stored once, referenced by uint32_t ID.
     *
     *   pool.Intern("mutex_lock") -> 42
     *   pool.Resolve(42)          -> "mutex_lock"
     *
     * Internals: contiguous char buffer (m_data) + null termination + hashmap (string -> offset).
     * ID 0 is reserved for the empty string (null sentinel).
     *
     * Intern map keys use std::string copies (prevents dangling on m_data realloc).
     * At 7.5M symbols, ~500K unique strings, avg 15 bytes each = ~7.5MB extra — negligible.
     */
    class StringPool
    {
    public:
        StringPool();

        /**
         * Register a string in the pool and return its ID.
         * Returns the existing ID in O(1) if the string is already registered.
         */
        uint32_t Intern(std::string_view s);

        /**
         * Resolve ID -> original string in O(1).
         * @param id  Offset returned by Intern
         */
        std::string_view Resolve(uint32_t id) const;

        /**
         * Return the ID if the string is already interned, otherwise 0.
         * Unlike Intern(), this is a const lookup that does not register new strings.
         */
        uint32_t Find(std::string_view s) const;

        /** Number of registered unique strings */
        size_t UniqueCount() const { return m_intern.size(); }

        /** Total byte usage of the pool (char buffer size) */
        size_t BytesUsed() const { return m_data.size(); }

        /** Clear the pool */
        void Clear();

        /** Pre-allocate for the expected number of strings (minimizes realloc) */
        void Reserve(size_t estimatedBytes, size_t estimatedStrings);

    private:
        /** Contiguous char buffer: [str0\0str1\0str2\0...] */
        std::vector<char> m_data;

        /** String -> offset reverse index (key = std::string copy, realloc-safe) */
        std::unordered_map<std::string, uint32_t> m_intern;
    };

} // namespace fce

#include "Baked/StringPool.hpp"
#include <cassert>

namespace fce
{

StringPool::StringPool()
{
    // ID 0 = empty string sentinel
    m_data.push_back('\0');
}

uint32_t StringPool::Intern(std::string_view s)
{
    if (s.empty()) return 0;

    // O(1) check if already interned
    auto it = m_intern.find(std::string(s));
    if (it != m_intern.end())
        return it->second;

    // New string: current offset becomes the ID
    const auto offset = static_cast<uint32_t>(m_data.size());
    m_data.insert(m_data.end(), s.begin(), s.end());
    m_data.push_back('\0');

    m_intern.emplace(std::string(s), offset);
    return offset;
}

std::string_view StringPool::Resolve(uint32_t id) const
{
    assert(id < m_data.size() && "StringPool::Resolve — invalid ID");
    return std::string_view(m_data.data() + id);
}

uint32_t StringPool::Find(std::string_view s) const
{
    if (s.empty()) return 0;
    auto it = m_intern.find(std::string(s));
    if (it != m_intern.end())
        return it->second;
    return 0;
}

void StringPool::Clear()
{
    m_data.clear();
    m_data.push_back('\0');
    m_intern.clear();
}

void StringPool::Reserve(size_t estimatedBytes, size_t estimatedStrings)
{
    m_data.reserve(estimatedBytes);
    m_intern.reserve(estimatedStrings);
}

} // namespace fce

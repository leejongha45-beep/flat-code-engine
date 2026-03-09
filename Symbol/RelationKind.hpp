#pragma once

#include <cstdint>

namespace fce
{

/** Relation kinds between symbols */
enum class RelationKind
{
    Inherits,   ///< Derived → Base (inheritance)
    DerivedBy,  ///< Base → Derived (reverse inheritance)
    MemberOf,   ///< Foo() → MyClass (class membership)
    HasMember,  ///< MyClass → Foo() (reverse membership)
    DefinedIn,  ///< symbol → file_path (file membership)
    Contains,   ///< file_path → symbol (reverse file membership)
    Includes,   ///< file_path → included_file_path (#include relation)
    IncludedBy, ///< included_file_path → file_path (reverse #include)
    Calls,      ///< A → B (A calls B)
    CalledBy,   ///< B → A (B is called by A)
    Returns,    ///< function → return type (CreateWidget → Widget)
    ReturnedBy, ///< return type → function (Widget → CreateWidget)
    TypedAs,    ///< function → parameter type (mutex_lock → mutex)
    TypeOf,     ///< parameter type → function (mutex → mutex_lock)
};

// ── BFS relation filter bitmask ─────────────────────────────────────

using RelationMask = uint32_t;
constexpr RelationMask kAllRelations = 0;   ///< 0 = filter OFF (traverse all)

/** RelationKind → bit conversion */
inline RelationMask RelationBit(RelationKind k)
{
    return 1u << static_cast<uint32_t>(k);
}

/** Semantic edges only (excludes DefinedIn/Contains/Includes/IncludedBy) */
constexpr RelationMask kSemanticRelations =
    (1u << static_cast<uint32_t>(RelationKind::Inherits))  |
    (1u << static_cast<uint32_t>(RelationKind::DerivedBy)) |
    (1u << static_cast<uint32_t>(RelationKind::MemberOf))  |
    (1u << static_cast<uint32_t>(RelationKind::HasMember)) |
    (1u << static_cast<uint32_t>(RelationKind::Calls))     |
    (1u << static_cast<uint32_t>(RelationKind::CalledBy))  |
    (1u << static_cast<uint32_t>(RelationKind::Returns))   |
    (1u << static_cast<uint32_t>(RelationKind::ReturnedBy))|
    (1u << static_cast<uint32_t>(RelationKind::TypedAs))   |
    (1u << static_cast<uint32_t>(RelationKind::TypeOf));

/** RelationKind reverse mapping (Calls → CalledBy, Inherits → DerivedBy, etc.) */
constexpr RelationKind InverseRelation(RelationKind k)
{
    switch (k)
    {
    case RelationKind::Inherits:   return RelationKind::DerivedBy;
    case RelationKind::DerivedBy:  return RelationKind::Inherits;
    case RelationKind::MemberOf:   return RelationKind::HasMember;
    case RelationKind::HasMember:  return RelationKind::MemberOf;
    case RelationKind::DefinedIn:  return RelationKind::Contains;
    case RelationKind::Contains:   return RelationKind::DefinedIn;
    case RelationKind::Includes:   return RelationKind::IncludedBy;
    case RelationKind::IncludedBy: return RelationKind::Includes;
    case RelationKind::Calls:      return RelationKind::CalledBy;
    case RelationKind::CalledBy:   return RelationKind::Calls;
    case RelationKind::Returns:    return RelationKind::ReturnedBy;
    case RelationKind::ReturnedBy: return RelationKind::Returns;
    case RelationKind::TypedAs:    return RelationKind::TypeOf;
    case RelationKind::TypeOf:     return RelationKind::TypedAs;
    }
    return k;
}

} // namespace fce

#pragma once
/**
 * IFceObject.hpp — FCE subsystem lifecycle interface
 *
 * Lifecycle:
 *   Initialize() — allocate resources
 *   Update(dt)   — periodic state update (optional)
 *   Release()    — release resources (reverse order of Initialize)
 */

namespace fce
{

class IFceObject
{
public:
    virtual void Initialize()         = 0;
    virtual void Update(float /*dt*/) {}
    virtual void Release()            = 0;
    virtual ~IFceObject()             = default;
};

} // namespace fce

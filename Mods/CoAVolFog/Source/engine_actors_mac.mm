// engine_actors.cpp of the Windows CoAVolFog on macOS: the units standing or swimming in water near the camera
// target, for the ripples. The Windows build walks the object manager's visible list through fixed offsets of
// Wow.exe (guarded by the code bytes that use them); here the client's own types give the same fields:
//
//   object manager 0xC79CE0 + 0x2ED0, visible list 0xA4..0xAC   ClntObjMgrGetCurrent()->m_visibleObjects
//   object guid +0x30, type mask descriptors + 0x08             CGObject_C::m_key.m_guid, m_obj->m_type
//   unit transport guid 0x790, position 0x798, flags 0x7CC      CGUnit_C::m_movementData (CPassenger, CMovementShared)
//   unit collision radius 0x850, height 0x854                   CMovement_C::m_collisionRadius, m_collisionHeight
//   object world entity +0xB8                                   CGObject_C::m_worldObject
//   entity position +0x6C, liquid flags +0x7C, surface +0x80    CMapStaticEntity::m_position, m_staticFlags, m_liquidHeight
//
// The selection (SelectWaterContact and its plausibility checks) is the original's unchanged.

#include "engine_actors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/WorldClient/MapLoad.h"
#include "WoW/Source/WorldClient/MapEntity.h"

namespace
{
constexpr uint32_t kMaxVisibleObjects = 4096;
constexpr uint32_t kSwimmingMovementFlag = 0x200000;
constexpr uint32_t kEntityLiquidFound = 0x20;
constexpr uint32_t kEntitySurfaceCrossesBody = 0x100;
constexpr float kMaxWorldCoordinate = 100000.0f;
constexpr float kMaxCollisionRadius = 50.0f;
constexpr float kMaxCollisionHeight = 100.0f;
constexpr float kMinClientRippleDepthLimit = 1.0f;
constexpr float kClientRippleDepthLimitPerHeight = 2.0f;

bool FiniteWithin(float value, float minimum, float maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool PlausibleContact(const WaterContact& c)
{
    for (float axis : c.position)
        if (!FiniteWithin(axis, -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return FiniteWithin(c.surface, -kMaxWorldCoordinate, kMaxWorldCoordinate) && c.radius > 0.0f &&
           FiniteWithin(c.radius, 0.0f, kMaxCollisionRadius) && c.height > 0.0f &&
           FiniteWithin(c.height, 0.0f, kMaxCollisionHeight) && WaterContactDepth(c) < ClientRippleDepthLimit(c.height);
}

double SquaredGroundDistance(const WaterContact& c, const float centre[3])
{
    const double dx = static_cast<double>(c.position[0]) - centre[0];
    const double dy = static_cast<double>(c.position[1]) - centre[1];
    return dx * dx + dy * dy;
}

bool Precedes(const WaterContact& a, const WaterContact& b, const float centre[3])
{
    const double aDistance = SquaredGroundDistance(a, centre);
    const double bDistance = SquaredGroundDistance(b, centre);
    if (aDistance != bDistance)
        return aDistance < bDistance;
    return a.guid < b.guid;
}

bool ValidCentre(const float centre[3])
{
    for (int i = 0; i < 3; ++i)
        if (!FiniteWithin(centre[i], -kMaxWorldCoordinate, kMaxWorldCoordinate))
            return false;
    return true;
}

// ReadWaterContactUnsafe: a unit whose world entity found liquid crossing its body
bool ReadWaterContact(CGObject_C* object, WaterContact& contact)
{
    if (!object->m_obj || !(object->m_obj->m_type & TYPEMASK_UNIT))
        return false;
    const CGUnit_C* unit = static_cast<const CGUnit_C*>(object);
    const CMapEntity* entity = object->m_worldObject;
    if (!entity)
        return false;
    const uint32_t liquid = entity->m_staticFlags;
    if (!(liquid & kEntityLiquidFound) || !(liquid & kEntitySurfaceCrossesBody))
        return false;
    const CMovement_C& movement = unit->m_movementData;
    contact.guid = object->m_key.m_guid.m_guid;
    contact.onTransport = movement.m_transportGuid.m_guid != 0;
    const C3Vector& position = contact.onTransport ? entity->m_position : movement.m_position;
    contact.position[0] = position.x;
    contact.position[1] = position.y;
    contact.position[2] = position.z;
    contact.surface = entity->m_liquidHeight;
    contact.radius = movement.m_collisionRadius;
    contact.height = movement.m_collisionHeight;
    contact.swimming = (movement.m_flags & kSwimmingMovementFlag) != 0;
    return true;
}
}

float WaterContactDepth(const WaterContact& contact)
{
    return contact.surface - contact.position[2];
}

float ClientRippleDepthLimit(float height)
{
    return std::max(kMinClientRippleDepthLimit, kClientRippleDepthLimitPerHeight * height);
}

namespace engine
{
bool SelectWaterContact(WaterContactFrame& frame, const WaterContact& contact, const float centre[3])
{
    if (frame.count > kMaxWaterContacts || !ValidCentre(centre) || !PlausibleContact(contact) ||
        SquaredGroundDistance(contact, centre) > static_cast<double>(kWaterContactRange) * kWaterContactRange)
        return false;
    uint32_t index = 0;
    while (index < frame.count && !Precedes(contact, frame.contacts[index], centre))
        ++index;
    if (index == kMaxWaterContacts)
        return false;
    const uint32_t count = std::min(frame.count + 1, kMaxWaterContacts);
    for (uint32_t i = count - 1; i > index; --i)
        frame.contacts[i] = frame.contacts[i - 1];
    frame.contacts[index] = contact;
    frame.count = count;
    return true;
}

// The object manager is the client's current one (ObjectMgr*); the address form of the Windows build is unused
bool CaptureWaterContactsFrom(uintptr_t objectManager, const float centre[3], WaterContactFrame& out)
{
    out = {};
    if (!ValidCentre(centre))
        return false;
    ObjectMgr* manager = reinterpret_cast<ObjectMgr*>(objectManager);
    if (!manager)
        return true;
    uint32_t visited = 0;
    for (CGObject_C* object = manager->m_visibleObjects.Head(); object; object = manager->m_visibleObjects.Next(object))
    {
        if (visited++ == kMaxVisibleObjects)
        {
            out.truncated = true;
            break;
        }
        WaterContact contact;
        if (ReadWaterContact(object, contact))
            SelectWaterContact(out, contact, centre);
    }
    return true;
}

bool WaterContactsSupported()
{
    return true;
}

bool CaptureWaterContacts(const float centre[3], WaterContactFrame& out)
{
    return CaptureWaterContactsFrom(reinterpret_cast<uintptr_t>(ClntObjMgrGetCurrent()), centre, out);
}
}

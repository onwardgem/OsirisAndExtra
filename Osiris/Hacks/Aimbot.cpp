#include "Aimbot.h"
#include "../Config.h"
#include "../Interfaces.h"
#include "../Memory.h"
#include "../SDK/Angle.h"
#include "../SDK/ConVar.h"
#include "../SDK/Entity.h"
#include "../SDK/UserCmd.h"
#include "../SDK/Vector.h"
#include "../SDK/WeaponId.h"
#include "../SDK/GlobalVars.h"
#include "../SDK/PhysicsSurfaceProps.h"
#include "../SDK/WeaponData.h"
#include "../SDK/ModelInfo.h"
#include "Animations.h"

Vector Aimbot::calculateRelativeAngle(const Vector& source, const Vector& destination, const Vector& viewAngles) noexcept
{
    return ((destination - source).toAngle() - viewAngles).normalize();
}

static bool traceToExit(const Trace& enterTrace, const Vector& start, const Vector& direction, Vector& end, Trace& exitTrace)
{
    bool result = false;
    const auto traceToExitFn = memory->traceToExit;
    __asm {
        push exitTrace
        mov eax, direction
        push [eax]Vector.z
        push [eax]Vector.y
        push [eax]Vector.x
        mov eax, start
        push [eax]Vector.z
        push [eax]Vector.y
        push [eax]Vector.x
        mov edx, enterTrace
        mov ecx, end
        call traceToExitFn
        add esp, 28
        mov result, al
    }
    return result;
}

static float handleBulletPenetration(SurfaceData* enterSurfaceData, const Trace& enterTrace, const Vector& direction, Vector& result, float penetration, float damage) noexcept
{
    Vector end;
    Trace exitTrace;

    if (!traceToExit(enterTrace, enterTrace.endpos, direction, end, exitTrace))
        return -1.0f;

    SurfaceData* exitSurfaceData = interfaces->physicsSurfaceProps->getSurfaceData(exitTrace.surface.surfaceProps);

    float damageModifier = 0.16f;
    float penetrationModifier = (enterSurfaceData->penetrationmodifier + exitSurfaceData->penetrationmodifier) / 2.0f;

    if (enterSurfaceData->material == 71 || enterSurfaceData->material == 89) {
        damageModifier = 0.05f;
        penetrationModifier = 3.0f;
    } else if (enterTrace.contents >> 3 & 1 || enterTrace.surface.flags >> 7 & 1) {
        penetrationModifier = 1.0f;
    }

    if (enterSurfaceData->material == exitSurfaceData->material) {
        if (exitSurfaceData->material == 85 || exitSurfaceData->material == 87)
            penetrationModifier = 3.0f;
        else if (exitSurfaceData->material == 76)
            penetrationModifier = 2.0f;
    }

    damage -= 11.25f / penetration / penetrationModifier + damage * damageModifier + (exitTrace.endpos - enterTrace.endpos).squareLength() / 24.0f / penetrationModifier;

    result = exitTrace.endpos;
    return damage;
}

bool Aimbot::canScan(Entity* entity, const Vector& destination, const WeaponInfo* weaponData, int minDamage, bool allowFriendlyFire) noexcept
 {
    if (!localPlayer)
        return false;

    float damage{ static_cast<float>(weaponData->damage) };

    Vector start{ localPlayer->getEyePosition() };
    Vector direction{ destination - start };
    direction /= direction.length();

    int hitsLeft = 4;

    while (damage >= 1.0f && hitsLeft) {
        Trace trace;
        interfaces->engineTrace->traceRay({ start, destination }, 0x4600400B, localPlayer.get(), trace);

        if (!allowFriendlyFire && trace.entity && trace.entity->isPlayer() && !localPlayer->isOtherEnemy(trace.entity))
            return false;

        if (trace.fraction == 1.0f)
            break;

        if (trace.entity == entity && trace.hitgroup > HitGroup::Generic && trace.hitgroup <= HitGroup::RightLeg) {
            damage = HitGroup::getDamageMultiplier(trace.hitgroup) * damage * powf(weaponData->rangeModifier, trace.fraction * weaponData->range / 500.0f);

            if (float armorRatio{ weaponData->armorRatio / 2.0f }; HitGroup::isArmored(trace.hitgroup, trace.entity->hasHelmet()))
                damage -= (trace.entity->armor() < damage * armorRatio / 2.0f ? trace.entity->armor() * 4.0f : damage) * (1.0f - armorRatio);

            return damage >= minDamage;
        }
        const auto surfaceData = interfaces->physicsSurfaceProps->getSurfaceData(trace.surface.surfaceProps);

        if (surfaceData->penetrationmodifier < 0.1f)
            break;

        damage = handleBulletPenetration(surfaceData, trace, direction, start, weaponData->penetration, damage);
        hitsLeft--;
    }
    return false;
}

float Aimbot::getScanDamage(Entity* entity, const Vector& destination, const WeaponInfo* weaponData, int minDamage, bool allowFriendlyFire) noexcept
{
    if (!localPlayer)
        return 0.f;

    float damage{ static_cast<float>(weaponData->damage) };

    Vector start{ localPlayer->getEyePosition() };
    Vector direction{ destination - start };
    direction /= direction.length();

    int hitsLeft = 4;

    while (damage >= 1.0f && hitsLeft) {
        Trace trace;
        interfaces->engineTrace->traceRay({ start, destination }, 0x4600400B, localPlayer.get(), trace);

        if (!allowFriendlyFire && trace.entity && trace.entity->isPlayer() && !localPlayer->isOtherEnemy(trace.entity))
            return 0.f;

        if (trace.fraction == 1.0f)
            break;

        if (trace.entity == entity && trace.hitgroup > HitGroup::Generic && trace.hitgroup <= HitGroup::RightLeg) {
            damage = HitGroup::getDamageMultiplier(trace.hitgroup) * damage * powf(weaponData->rangeModifier, trace.fraction * weaponData->range / 500.0f);

            if (float armorRatio{ weaponData->armorRatio / 2.0f }; HitGroup::isArmored(trace.hitgroup, trace.entity->hasHelmet()))
                damage -= (trace.entity->armor() < damage * armorRatio / 2.0f ? trace.entity->armor() * 4.0f : damage) * (1.0f - armorRatio);
           
            if (damage >= minDamage)
                return damage;
            return 0.f;
        }
        const auto surfaceData = interfaces->physicsSurfaceProps->getSurfaceData(trace.surface.surfaceProps);

        if (surfaceData->penetrationmodifier < 0.1f)
            break;

        damage = handleBulletPenetration(surfaceData, trace, direction, start, weaponData->penetration, damage);
        hitsLeft--;
    }
    return 0.f;
}

float segmentToSegment(const Vector& s1, const Vector& s2, const Vector& k1, const Vector& k2)
{
    static auto constexpr epsilon = 0.00000001f;

    auto u = s2 - s1;
    auto v = k2 - k1;
    auto w = s1 - k1;

    auto a = u.dotProduct(u); //-V525
    auto b = u.dotProduct(v);
    auto c = v.dotProduct(v);
    auto d = u.dotProduct(w);
    auto e = v.dotProduct(w);
    auto D = a * c - b * b;

    auto sn = 0.0f, sd = D;
    auto tn = 0.0f, td = D;

    if (D < epsilon)
    {
        sn = 0.0f;
        sd = 1.0f;
        tn = e;
        td = c;
    }
    else
    {
        sn = b * e - c * d;
        tn = a * e - b * d;

        if (sn < 0.0f)
        {
            sn = 0.0f;
            tn = e;
            td = c;
        }
        else if (sn > sd)
        {
            sn = sd;
            tn = e + b;
            td = c;
        }
    }

    if (tn < 0.0f)
    {
        tn = 0.0f;

        if (-d < 0.0f)
            sn = 0.0f;
        else if (-d > a)
            sn = sd;
        else
        {
            sn = -d;
            sd = a;
        }
    }
    else if (tn > td)
    {
        tn = td;

        if (-d + b < 0.0f)
            sn = 0.0f;
        else if (-d + b > a)
            sn = sd;
        else
        {
            sn = -d + b;
            sd = a;
        }
    }

    auto sc = fabs(sn) < epsilon ? 0.0f : sn / sd;
    auto tc = fabs(tn) < epsilon ? 0.0f : tn / td;

    auto dp = w + u * sc - v * tc;
    return dp.length();
}

bool intersectLineWithBb(Vector& start, Vector& end, Vector& min, Vector& max) {
    float d1, d2, f;
    auto start_solid = true;
    auto t1 = -1.0f, t2 = 1.0f;

    const float s[3] = { start.x, start.y, start.z };
    const float e[3] = { end.x, end.y, end.z };
    const float mi[3] = { min.x, min.y, min.z };
    const float ma[3] = { max.x, max.y, max.z };

    for (auto i = 0; i < 6; i++) {
        if (i >= 3) {
            const auto j = i - 3;

            d1 = s[j] - ma[j];
            d2 = d1 + e[j];
        }
        else {
            d1 = -s[i] + mi[i];
            d2 = d1 - e[i];
        }

        if (d1 > 0.0f && d2 > 0.0f)
            return false;

        if (d1 <= 0.0f && d2 <= 0.0f)
            continue;

        if (d1 > 0)
            start_solid = false;

        if (d1 > d2) {
            f = d1;
            if (f < 0.0f)
                f = 0.0f;

            f /= d1 - d2;
            if (f > t1)
                t1 = f;
        }
        else {
            f = d1 / (d1 - d2);
            if (f < t2)
                t2 = f;
        }
    }

    return start_solid || (t1 < t2&& t1 >= 0.0f);
}

void inline sinCos(float radians, float* sine, float* cosine)
{
    *sine = sin(radians);
    *cosine = cos(radians);
}

Vector vectorRotate(Vector& in1, Vector& in2)
{
    auto vector_rotate = [](const Vector& in1, const matrix3x4& in2)
    {
        return Vector(in1.dotProduct(in2[0]), in1.dotProduct(in2[1]), in1.dotProduct(in2[2]));
    };
    auto angleMatrix = [](const Vector& angles, matrix3x4& matrix)
    {
        float sr, sp, sy, cr, cp, cy;

        sinCos(Helpers::deg2rad(angles[1]), &sy, &cy);
        sinCos(Helpers::deg2rad(angles[0]), &sp, &cp);
        sinCos(Helpers::deg2rad(angles[2]), &sr, &cr);

        // matrix = (YAW * PITCH) * ROLL
        matrix[0][0] = cp * cy;
        matrix[1][0] = cp * sy;
        matrix[2][0] = -sp;

        float crcy = cr * cy;
        float crsy = cr * sy;
        float srcy = sr * cy;
        float srsy = sr * sy;
        matrix[0][1] = sp * srcy - crsy;
        matrix[1][1] = sp * srsy + crcy;
        matrix[2][1] = sr * cp;

        matrix[0][2] = (sp * crcy + srsy);
        matrix[1][2] = (sp * crsy - srcy);
        matrix[2][2] = cr * cp;

        matrix[0][3] = 0.0f;
        matrix[1][3] = 0.0f;
        matrix[2][3] = 0.0f;
    };
    matrix3x4 m;
    angleMatrix(in2, m);
    return vector_rotate(in1, m);
}

void vectorITransform(const Vector& in1, const matrix3x4& in2, Vector& out)
{
    out.x = (in1.x - in2[0][3]) * in2[0][0] + (in1.y - in2[1][3]) * in2[1][0] + (in1.z - in2[2][3]) * in2[2][0];
    out.y = (in1.x - in2[0][3]) * in2[0][1] + (in1.y - in2[1][3]) * in2[1][1] + (in1.z - in2[2][3]) * in2[2][1];
    out.z = (in1.x - in2[0][3]) * in2[0][2] + (in1.y - in2[1][3]) * in2[1][2] + (in1.z - in2[2][3]) * in2[2][2];
}

void vectorIRotate(Vector in1, matrix3x4 in2, Vector& out)
{
    out.x = in1.x * in2[0][0] + in1.y * in2[1][0] + in1.z * in2[2][0];
    out.y = in1.x * in2[0][1] + in1.y * in2[1][1] + in1.z * in2[2][1];
    out.z = in1.x * in2[0][2] + in1.y * in2[1][2] + in1.z * in2[2][2];
}

bool hitboxIntersection(Entity* entity, matrix3x4 matrix[256], int iHitbox, StudioHitboxSet* set, const Vector& start, const Vector& end)
{
    auto VectorTransform_Wrapper = [](const Vector& in1, const matrix3x4 in2, Vector& out)
    {
        auto VectorTransform = [](const float* in1, const matrix3x4 in2, float* out)
        {
            auto DotProducts = [](const float* v1, const float* v2)
            {
                return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
            };
            out[0] = DotProducts(in1, in2[0]) + in2[0][3];
            out[1] = DotProducts(in1, in2[1]) + in2[1][3];
            out[2] = DotProducts(in1, in2[2]) + in2[2][3];
        };
        VectorTransform(&in1.x, in2, &out.x);
    };

    StudioBbox* hitbox = set->getHitbox(iHitbox);
    if (!hitbox)
        return false;

    if (hitbox->capsuleRadius == -1.f)
        return false;

    Vector mins, maxs;
    const auto isCapsule = hitbox->capsuleRadius != -1.f;
    if (isCapsule)
    {
        VectorTransform_Wrapper(hitbox->bbMin, matrix[hitbox->bone], mins);
        VectorTransform_Wrapper(hitbox->bbMax, matrix[hitbox->bone], maxs);
        const auto dist = segmentToSegment(start, end, mins, maxs);

        if (dist < hitbox->capsuleRadius)
            return true;
    }
    else
    {
        VectorTransform_Wrapper(vectorRotate(hitbox->bbMin, hitbox->offsetOrientation), matrix[hitbox->bone], mins);
        VectorTransform_Wrapper(vectorRotate(hitbox->bbMax, hitbox->offsetOrientation), matrix[hitbox->bone], maxs);

        vectorITransform(start, matrix[hitbox->bone], mins);
        vectorIRotate(end, matrix[hitbox->bone], maxs);

        if (intersectLineWithBb(mins, maxs, hitbox->bbMin, hitbox->bbMax))
            return true;
    }
    return false;
}

std::vector<Vector> Aimbot::multiPoint(Entity* entity, matrix3x4 matrix[256], StudioBbox* hitbox, Vector localEyePos, int _hitbox, int _multiPoint)
{
    auto AngleVectors = [](const Vector& angles, Vector* forward)
    {
        float	sp, sy, cp, cy;

        sy = sin(Helpers::deg2rad(angles.y));
        cy = cos(Helpers::deg2rad(angles.y));

        sp = sin(Helpers::deg2rad(angles.x));
        cp = cos(Helpers::deg2rad(angles.x));

        forward->x = cp * cy;
        forward->y = cp * sy;
        forward->z = -sp;
    };

    auto VectorTransform_Wrapper = [](const Vector& in1, const matrix3x4 in2, Vector& out)
    {
        auto VectorTransform = [](const float* in1, const matrix3x4 in2, float* out)
        {
            auto DotProducts = [](const float* v1, const float* v2)
            {
                return v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
            };
            out[0] = DotProducts(in1, in2[0]) + in2[0][3];
            out[1] = DotProducts(in1, in2[1]) + in2[1][3];
            out[2] = DotProducts(in1, in2[2]) + in2[2][3];
        };
        VectorTransform(&in1.x, in2, &out.x);
    };

    Vector vMin, vMax, vCenter;
    VectorTransform_Wrapper(hitbox->bbMin, matrix[hitbox->bone], vMin);
    VectorTransform_Wrapper(hitbox->bbMax, matrix[hitbox->bone], vMax);
    vCenter = (vMin + vMax) * 0.5f;

    std::vector<Vector> vecArray;

    if (_multiPoint <= 0)
    {
        vecArray.emplace_back(vCenter);
        return vecArray;
    }
    vecArray.emplace_back(vCenter);

    Vector CurrentAngles = Aimbot::calculateRelativeAngle(vCenter, localEyePos, Vector{});

    Vector Forward;
    AngleVectors(CurrentAngles, &Forward);

    Vector Right = Forward.Cross(Vector{ 0, 0, 1 });
    Vector Left = Vector{ -Right.x, -Right.y, Right.z };

    Vector Top = Vector{ 0, 0, 1 };
    Vector Bot = Vector{ 0, 0, -1 };

    float multiPoint = (min(_multiPoint, 95)) * 0.01f;

    switch (_hitbox)
    {
    case Hitboxes::Head:
        for (auto i = 0; i < 4; ++i)
            vecArray.emplace_back(vCenter);

        vecArray[1] += Top * (hitbox->capsuleRadius * multiPoint);
        vecArray[2] += Right * (hitbox->capsuleRadius * multiPoint);
        vecArray[3] += Left * (hitbox->capsuleRadius * multiPoint);
        break;
    default://rest
        for (auto i = 0; i < 3; ++i)
            vecArray.emplace_back(vCenter);

        vecArray[1] += Right * (hitbox->capsuleRadius * multiPoint);
        vecArray[2] += Left * (hitbox->capsuleRadius * multiPoint);
        break;
    }
    return vecArray;
}

bool Aimbot::hitChance(Entity* localPlayer, Entity* entity, StudioHitboxSet* set, matrix3x4 matrix[256], Entity* activeWeapon, const Vector& destination, const UserCmd* cmd, const int hitChance) noexcept
{
    static auto isSpreadEnabled = interfaces->cvar->findVar("weapon_accuracy_nospread");
    if (!hitChance || isSpreadEnabled->getInt() >= 1)
        return true;

    constexpr int maxSeed = 255;

    const Angle angles(destination + cmd->viewangles);

    int hits = 0;
    const int hitsNeed = static_cast<int>(static_cast<float>(maxSeed) * (static_cast<float>(hitChance) / 100.f));

    const auto weapSpread = activeWeapon->getSpread();
    const auto weapInaccuracy = activeWeapon->getInaccuracy();
    const auto localEyePosition = localPlayer->getEyePosition();
    const auto range = activeWeapon->getWeaponData()->range;

    for (int i = 0; i < maxSeed; i++)
    {
        srand(i + 1);
        const float spreadX = memory->randomFloat(0.f, 2.f * static_cast<float>(M_PI));
        const float spreadY = memory->randomFloat(0.f, 2.f * static_cast<float>(M_PI));
        auto inaccuracy = weapInaccuracy * memory->randomFloat(0.f, 1.f);
        auto spread = weapSpread * memory->randomFloat(0.f, 1.f);

        Vector spreadView{ (cosf(spreadX) * inaccuracy) + (cosf(spreadY) * spread),
                           (sinf(spreadX) * inaccuracy) + (sinf(spreadY) * spread) };
        Vector direction{ (angles.forward + (angles.right * spreadView.x) + (angles.up * spreadView.y)) * range };

        for (int hitbox = 0; hitbox < Hitboxes::Max; hitbox++)
        {
            if (hitboxIntersection(entity, matrix, hitbox, set, localEyePosition, localEyePosition + direction))
            {
                hits++;
                break;
            }
        }

        if (hits >= hitsNeed)
            return true;

        if ((maxSeed - i + hits) < hitsNeed)
            return false;
    }
    return false;
}
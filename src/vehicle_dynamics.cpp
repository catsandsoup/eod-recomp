// Native eight-substep vehicle dynamics used by the EE producer at 0x1DE7B0.
// Data layout comes from the three contiguous uploads at 0x1DE8D8..0x1DEA10.
#include "vehicle_dynamics.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {
constexpr float dt = 0.0166f * 0.125f;
struct RigidBody {
    VUReg position, axis[3], rotation, linearVelocity, angularVelocity, reserved;
    VUReg inverseInertia, force, torque; // inverseInertia.w is inverse mass.
};
struct WheelSet {
    VUReg travel, angle, travelRate, spinRate, hub[4], driveState, slip[4], force[4];
    VUReg groundNormal[4], groundPoint[4], driveControl, brakeControl, reserved[4];
};
struct Tuning {
    VUReg attachment[4], inverseSuspensionMass, inverseWheelInertia, airResistance, radius;
    VUReg contactStiffness, contactDamping, grip, reserved11, spring, damperPositive, damperNegative;
    VUReg antiRoll, steerGain, steerOffset, reserved18, driveSplit, brake, reserved21, differential, reserved23;
};
struct Records {
    RigidBody body;
    WheelSet wheels;
    Tuning tuning;
};
static_assert(sizeof(RigidBody) == 11 * 16 && sizeof(WheelSet) == 31 * 16 && sizeof(Tuning) == 24 * 16);
struct Vec {
    float x[3];
    float& operator[](int i) { return x[i]; }
    float operator[](int i) const { return x[i]; }
};
struct Solver {

    uint32_t subnormal{};
    bool divideZero{};
    float observe(float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        u &= 0x7fffffffu;
        subnormal |= unsigned(u - 1u < 0x7fffffu);
        return f;
    }
    float clamp(float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        if ((u & 0x7f800000) != 0x7f800000)
            return f;
        u = (u & 0x80000000) | 0x7f7fffff;
        memcpy(&f, &u, 4);
        return f;
    }
    float add(float a, float b) {
        float v = a + b;
        return observe(std::isfinite(v) ? v : clamp(clamp(a) + clamp(b)));
    }
    float sub(float a, float b) {
        float v = a - b;
        return observe(std::isfinite(v) ? v : clamp(clamp(a) - clamp(b)));
    }
    float mul(float a, float b) {
        float v = a * b;
        return observe(std::isfinite(v) ? v : clamp(clamp(a) * clamp(b)));
    }
    float madd(float acc, float a, float b) {
        float v = acc + a * b;
        return observe(std::isfinite(v) ? v : clamp(clamp(acc) + clamp(clamp(a) * clamp(b))));
    }
    float bounded(float v, float low, float high) {
        v = clamp(v) < high ? v : high;
        return clamp(v) > low ? v : low;
    }
    void integrate(RigidBody& body, VUState* out) {

        float *pos = body.position.f, *quat = body.rotation.f, *linear = body.linearVelocity.f,
              *angular = body.angularVelocity.f;
        float *inverse = body.inverseInertia.f, *force = body.force.f, *torque = body.torque.f;
        float local_torque[3], halfOmega[4];
        for (int i = 0; i < 3; i++) {
            const float* axis = body.axis[i].f;
            float p[3];
            for (int j = 0; j < 3; ++j)
                p[j] = mul(axis[j], torque[j]);
            local_torque[i] = mul(inverse[i], add(add(p[i], p[(i + 1) % 3]), p[(i + 2) % 3]));
        }
        for (int i = 0; i < 3; ++i) {
            float spin = madd(madd(mul(body.axis[0].f[i], local_torque[0]), body.axis[1].f[i], local_torque[1]),
                              body.axis[2].f[i], local_torque[2]);
            angular[i] = add(bounded(angular[i], -20, 20), mul(spin, dt));
            halfOmega[i] = mul(angular[i], 0.5f);
            float acceleration = mul(force[i], inverse[3]);
            if (i == 1)
                acceleration = sub(acceleration, 9.81f);
            linear[i] = add(bounded(linear[i], -200, 200), mul(acceleration, dt));
            pos[i] = add(pos[i], mul(linear[i], dt));
            force[i] = torque[i] = 0;
        }
        halfOmega[3] = 0.0f;
        // Integrate the quaternion derivative in the original product order.
        float product[4];
        for (int i = 0; i < 3; ++i) {
            const int j = (i + 1) % 3, k = (i + 2) % 3;
            float acc = observe(clamp(quat[j] * halfOmega[k]));
            acc = madd(acc, quat[i], halfOmega[3]);
            acc = madd(acc, halfOmega[i], quat[3]);
            product[i] = observe(clamp(acc - halfOmega[j] * quat[k]));
        }
        const float angularScalar =
            sub(sub(mul(quat[3], halfOmega[3]), mul(quat[2], halfOmega[2])), mul(quat[1], halfOmega[1]));
        product[3] = sub(angularScalar, mul(quat[0], halfOmega[0]));
        if (out)
            out->acc.f[3] = angularScalar;
        for (int i = 0; i < 4; ++i)
            quat[i] = sub(quat[i], mul(product[i], dt));
        float norm = mul(quat[0], quat[0]);
        for (int i = 1; i < 4; ++i)
            norm = add(norm, mul(quat[i], quat[i]));
        divideZero |= norm == 0.f;
        const float invnorm = norm == 0.0f ? 0x1.fffffep127f : clamp(1.0f / std::sqrt(std::fabs(clamp(norm))));
        float scaled[4];
        for (int i = 0; i < 4; ++i) {
            quat[i] = mul(quat[i], invnorm);
            scaled[i] = mul(quat[i], 1.41421356f);
        }
        // Rebuild the three basis columns from the normalized orientation.
        float plus[3], minus[3], twiceSquare[3];
        for (int i = 0; i < 3; ++i) {
            int j = (i + 1) % 3, k = (i + 2) % 3;
            float cross = observe(clamp(scaled[j] * scaled[k]));
            plus[i] = madd(cross, scaled[i], scaled[3]);
            float v = cross - scaled[i] * scaled[3];
            minus[i] = observe(
                std::isfinite(v) ? v : clamp(clamp(cross) - clamp(clamp(scaled[i]) * clamp(scaled[3]))));
            twiceSquare[i] = madd(mul(quat[i], quat[i]), quat[i], quat[i]);
        }
        body.axis[0].f[0] = sub(sub(1.0f, twiceSquare[1]), twiceSquare[2]);
        body.axis[0].f[1] = minus[2];
        body.axis[0].f[2] = add(0.0f, plus[1]);
        body.axis[1].f[0] = plus[2];
        body.axis[1].f[1] = sub(sub(1.0f, twiceSquare[0]), twiceSquare[2]);
        body.axis[1].f[2] = minus[0];
        body.axis[2].f[0] = minus[1];
        body.axis[2].f[1] = add(0.0f, plus[0]);
        body.axis[2].f[2] = sub(sub(1.0f, twiceSquare[0]), twiceSquare[1]);
    }

    Vec load(const VUReg& v) { return {{v.f[0], v.f[1], v.f[2]}}; }
    void store(VUReg& v, Vec data) {
        for (int i = 0; i < 3; i++)
            v.f[i] = data[i];
    }
    Vec plus(Vec a, Vec b) {
        for (int i = 0; i < 3; i++)
            a[i] = add(a[i], b[i]);
        return a;
    }
    Vec minus(Vec a, Vec b) {
        for (int i = 0; i < 3; i++)
            a[i] = sub(a[i], b[i]);
        return a;
    }
    Vec times(Vec a, float s) {
        for (int i = 0; i < 3; i++)
            a[i] = mul(a[i], s);
        return a;
    }
    float dot(Vec a, Vec b) {
        return add(add(mul(a[0], b[0]), mul(a[1], b[1])), mul(a[2], b[2]));
    }
    Vec cross(Vec a, Vec b) {
        Vec r;
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3, k = (i + 2) % 3;
            r[i] = observe(clamp(observe(clamp(a[j] * b[k])) - b[j] * a[k]));
        }
        return r;
    }
    float root(float x) { return sqrtf(fabsf(clamp(x))); }
    float invroot(float x) {
        divideZero |= x == 0.f;
        return x == 0 ? 0x1.fffffep127f : clamp(1.0f / root(x));
    }
    float ratio(float a, float b) {
        a = clamp(a);
        b = clamp(b);
        return b == 0 ? copysignf(0x1.fffffep127f, std::signbit(a) == std::signbit(b) ? 1.f : -1.f)
                      : clamp(a / b);
    }
    float minf(float a, float b) { return clamp(a) < clamp(b) ? a : b; }
    float maxf(float a, float b) { return clamp(a) > clamp(b) ? a : b; }
    float pairSum(float* x) { return add(add(x[1], x[0]), add(x[3], x[2])); }
    void wheel_contacts(RigidBody& body, WheelSet& wheels, const Tuning& tuning, VUState* out) {

        const Vec origin = load(body.position), right = load(body.axis[0]), up = load(body.axis[1]),
                  forward = load(body.axis[2]);
        const Vec velocity = load(body.linearVelocity), omega = load(body.angularVelocity);
        Vec forces = minus(load(body.force),
                           times(velocity, mul(root(dot(velocity, velocity)), tuning.airResistance.f[0])));
        Vec torques = load(body.torque);
        VUReg& travel = wheels.travel;
        VUReg& angle = wheels.angle;
        VUReg& travelRate = wheels.travelRate;
        VUReg& spin = wheels.spinRate;
        float drivenSpeed[4], weightedSpin[4], steer[4], spinTorque[4], spring[4];
        const float gearbox = wheels.driveControl.f[3];
        // Drivetrain feedback, per-wheel suspension and axle anti-roll coupling.
        for (int i = 0; i < 4; i++) {
            drivenSpeed[i] = mul(tuning.driveSplit.f[i], mul(spin.f[i], gearbox));
            weightedSpin[i] = mul(tuning.driveSplit.f[i], mul(spin.f[i], tuning.radius.f[i]));
            steer[i] = add(mul(tuning.steerGain.f[i], wheels.driveControl.f[0]), tuning.steerOffset.f[i]);
        }
        const float engage = minf(fabsf(mul(gearbox, 1000.f)), 1.0f);
        const float rotationDelta =
            mul(bounded(mul(sub(pairSum(drivenSpeed), wheels.driveState.f[0]), 1.0f), -1.f, 1.f),
                mul(900.f, mul(engage, wheels.driveControl.f[2])));
        const float differential = mul(sub(spin.f[3], spin.f[2]), tuning.differential.f[0]);
        wheels.driveState.f[1] = add(0, pairSum(weightedSpin));
        wheels.driveState.f[0] =
            add(wheels.driveState.f[0],
                mul(add(0, add(mul(wheels.driveState.f[3], .999f), rotationDelta)), mul(dt, 3.16f)));
        for (int i = 0; i < 4; i++) {
            const float brake =
                mul(mul(bounded(mul(spin.f[i], 4), -1, 1), tuning.brake.f[i]), wheels.brakeControl.f[i]);
            spinTorque[i] = sub(mul(tuning.driveSplit.f[i], mul(rotationDelta, mul(gearbox, -1.0f))), brake);
            const float sign = mul(fabsf(bounded(mul(travelRate.f[i], 100000.f), -1, 1)), -1.f);
            const float damping = sub(mul(tuning.damperPositive.f[i], maxf(sign, 0.f)),
                                      mul(tuning.damperNegative.f[i], minf(sign, 0.f)));
            spring[i] = sub(mul(travel.f[i], mul(tuning.spring.f[i], -1)), mul(travelRate.f[i], damping));
        }
        spinTorque[2] = add(spinTorque[2], differential);
        spinTorque[3] = sub(spinTorque[3], differential);
        for (int axle = 0; axle < 4; axle += 2) {
            const float bar = mul(sub(travel.f[axle + 1], travel.f[axle]), tuning.antiRoll.f[axle]);
            spring[axle] = add(spring[axle], bar);
            spring[axle + 1] = sub(spring[axle + 1], bar);
        }
        // Live-out lanes from contact setup, before the wheel loop updates the rates.
        if (out) {
            out->vf[3].f[3] = tuning.inverseSuspensionMass.f[3];
            out->vf[5].f[3] = mul(travelRate.f[3], 100000.f);
            out->vf[6] = tuning.contactStiffness;
            out->vf[7] = tuning.contactDamping;
            out->vf[8] = tuning.radius;
            out->vf[12].f[3] = mul(dt, 3.16f);
            for (int i = 0; i < 4; ++i)
                out->vf[13].f[i] = steer[i];
            float a = fabsf(bounded(mul(travelRate.f[3], 100000.f), -1, 1));
            float neg = mul(a, -1.f), pos = maxf(neg, 0.f);
            out->vf[16].f[3] = mul(tuning.damperPositive.f[3], pos);
            out->vf[17].f[3] = a;
            out->vf[18].f[3] = sub(
                mul(travel.f[3], mul(tuning.spring.f[3], -1.f)),
                mul(travelRate.f[3], sub(out->vf[16].f[3], mul(tuning.damperNegative.f[3], minf(neg, 0.f)))));
            out->vf[19].f[3] = pos;
            out->vf[20].f[3] = wheels.brakeControl.f[3];
            out->vf[23].f[3] = travelRate.f[3];
            out->vf[30].f[3] = spin.f[3];
        }
        for (int i = 0; i < 4; i++) {
            Vec direction = minus(right, times(forward, steer[i]));
            const float inverseLength = invroot(dot(direction, direction));
            direction = times(direction, inverseLength);
            const Vec attachment = load(tuning.attachment[i]);
            Vec arm;
            for (int j = 0; j < 3; j++)
                arm[j] =
                    madd(madd(mul(right[j], attachment[0]), up[j], attachment[1]), forward[j], attachment[2]);
            Vec hub = plus(arm, plus(origin, times(up, travel.f[i])));
            store(wheels.hub[i], hub);
            const Vec normal = load(wheels.groundNormal[i]), sample = load(wheels.groundPoint[i]);
            const Vec contact = minus(hub, times(normal, dot(normal, minus(hub, sample))));
            const Vec offset = minus(contact, hub);
            const Vec speed =
                plus(plus(velocity, cross(omega, minus(hub, origin))), times(up, travelRate.f[i]));
            const Vec rolling = plus(speed, cross(times(direction, spin.f[i]), offset));
            const float penetration = maxf(0.f, add(tuning.radius.f[i], dot(normal, offset)));
            const float fraction = minf(1.f, add(0.f, mul(penetration, 10000.f)));
            const Vec point = plus(hub, times(up, dot(offset, up)));
            Vec history = load(wheels.slip[i]);
            const float decay = maxf(fabsf(mul(spin.f[i], offset[1])), 2.f);
            Vec tangent = minus(rolling, times(normal, dot(normal, rolling)));
            history =
                times(plus(history, times(times(minus(tangent, times(history, decay)), 10.f), dt)), fraction);
            store(wheels.slip[i], history);
            const float normalSpeed = dot(normal, speed);
            const float loadForce =
                maxf(0.f, add(0.f, sub(mul(penetration, tuning.contactStiffness.f[i]),
                                       mul(normalSpeed, mul(tuning.contactDamping.f[i], fraction)))));
            const float coefficient = maxf(add(tuning.grip.f[0], mul(tuning.grip.f[1], loadForce)), .01f);
            const float normalGrip = mul(loadForce, add(0.f, coefficient));
            const float scaledGrip =
                ratio(add(tuning.grip.f[2], mul(tuning.grip.f[3], loadForce)), maxf(normalGrip, .01f));
            const Vec slip = times(history, add(0.f, scaledGrip));
            // The retail tire curve joins a cubic low-slip response to the reciprocal tail.
            const float slipMagnitude = maxf(add(0.f, root(dot(slip, slip))), .001f);
            wheels.slip[i].f[3] = mul(1.f, slipMagnitude);
            const float inverseSlip = add(0.f, ratio(1.f, slipMagnitude));
            const float weight = maxf(minf(mul(-100.f, sub(slipMagnitude, 2.68f)), 1.f), 0.f);
            const float slipSquared = mul(slipMagnitude, slipMagnitude);
            const float low = add(mul(slipSquared, mul(slipMagnitude, .0493f)),
                                  add(mul(slipSquared, -.403f), mul(slipMagnitude, 1.1f)));
            const float high =
                add(mul(-.9f, mul(inverseSlip, inverseSlip)), add(mul(.79f, inverseSlip), .833f));
            float curve = add(mul(weight, low), mul(sub(1.f, weight), high));
            const Vec tangentialForce = times(times(slip, mul(curve, inverseSlip)), normalGrip);
            const Vec friction = minus(Vec{}, tangentialForce);
            const Vec wheelForce = plus(times(normal, loadForce), friction);
            spinTorque[i] = add(spinTorque[i], dot(cross(offset, friction), direction));
            const float axialForce = dot(wheelForce, up);
            const Vec axial = times(up, axialForce);
            const Vec sideForce = minus(wheelForce, axial);
            const Vec bodyForce = minus(sideForce, times(up, spring[i]));
            const Vec lever = minus(point, origin);
            const Vec moment = cross(lever, bodyForce);
            store(wheels.force[i], wheelForce);
            // Final wheel temporaries visible to subsequent COP2 consumers.
            if (out && i == 3) {
                for (int j = 0; j < 3; ++j)
                    out->acc.f[j] = clamp(lever[(j + 1) % 3] * bodyForce[(j + 2) % 3]);
                store(out->vf[16], moment);
                store(out->vf[17], sideForce);
                store(out->vf[18], lever);
                out->vf[19].f[0] = add(spring[i], axialForce);
                out->vf[19].f[1] = mul(wheelForce[1], up[1]);
                out->vf[19].f[2] = mul(wheelForce[2], up[2]);
                store(out->vf[20], axial);
                out->vf[21].f[0] = mul(sub(1.f, weight), high);
                out->vf[21].f[1] = add(0.f, loadForce);
                out->vf[21].f[2] = add(0.f, high);
                out->vf[22] = tuning.grip;
                out->vf[22].f[0] = low;
                out->vf[22].f[1] = add(0.f, coefficient);
                store(out->vf[23], wheelForce);
                out->vf[24].f[3] = tuning.grip.f[3];
                store(out->vf[24], tangentialForce);
                out->vf[25].f[0] = high;
                out->vf[25].f[1] = add(0.f, curve);
                out->vf[25].f[2] = history[2];
                out->vf[26].f[0] = mul(-.9f, mul(inverseSlip, inverseSlip));
                out->vf[26].f[1] = minus(hub, origin)[1];
                out->vf[26].f[2] = minus(hub, origin)[2];
                out->vf[27] = tuning.contactDamping;
                out->vf[27].f[0] = add(mul(.79f, inverseSlip), .833f);
                out->vf[28].f[0] = mul(.79f, inverseSlip);
                out->vf[28].f[1] = tangent[1];
                out->vf[28].f[2] = tangent[2];
                out->vf[28].f[3] = tuning.radius.f[3];
                out->vf[29] = tuning.contactStiffness;
                out->vf[29].f[0] = 0.f;
                out->vf[30].f[0] = minf(mul(-100.f, sub(slipMagnitude, 2.68f)), 1.f);
                out->vf[30].f[1] = tangent[1];
                out->vf[30].f[2] = tangent[2];
                out->vf[31].f[0] = inverseSlip;
                out->vf[31].f[1] = rolling[1];
                out->vf[31].f[2] = rolling[2];
                out->q = out->q_pending = ratio(1.f, slipMagnitude);
            }
            spring[i] = add(spring[i], axialForce);
            forces = plus(forces, bodyForce);
            torques = plus(torques, moment);
        }
        for (int i = 0; i < 4; i++) {
            travelRate.f[i] = add(travelRate.f[i], mul(spring[i], mul(tuning.inverseSuspensionMass.f[i], dt)));
            spin.f[i] = add(spin.f[i], mul(spinTorque[i], mul(tuning.inverseWheelInertia.f[i], dt)));
            travel.f[i] = add(travel.f[i], mul(travelRate.f[i], dt));
            angle.f[i] = add(angle.f[i], mul(spin.f[i], dt));
            if (out) {
                out->vf[10].f[i] = travel.f[i];
                out->vf[11].f[i] = spin.f[i];
                out->vf[14].f[i] = travelRate.f[i];
                out->vf[15].f[i] = angle.f[i];
            }
        }
        if (out) {
            store(out->vf[3], torques);
            store(out->vf[4], forces);
            out->vf[4].f[3] = mul(spring[3], mul(tuning.inverseSuspensionMass.f[3], dt));
            store(out->vf[5], origin);
            store(out->vf[9], forces);
            out->vf[9].f[3] = spin.f[3];
            store(out->vf[12], forces);
            out->vf[1].f[3] = travel.f[3];
            out->vf[2].f[3] = mul(tuning.inverseWheelInertia.f[3], dt);
        }
        store(body.torque, torques);
        store(body.force, forces);
    }

    unsigned mac(const VUReg& r, unsigned mask) {
        unsigned result = 0;
        for (unsigned lane = 0; lane < 4; ++lane) {
            const unsigned bits = r.u[lane], magnitude = bits & 0x7fffffffu, bit = 8u >> lane;
            if (!(mask & bit))
                continue;
            if (!magnitude)
                result |= bit;
            if (bits >> 31)
                result |= bit << 4;
            if (magnitude - 1u < 0x7fffffu)
                result |= bit << 8;
            if (magnitude >= 0x7f800000u)
                result |= bit << 12;
        }
        return result;
    }

    void run(VUState& vu) {
        Records records;
        std::memcpy(&records, vu.mem, sizeof records);
        RigidBody& body = records.body;
        for (int j = 0; j < 3; ++j) {
            body.force.f[j] = mul(body.force.f[j], 8.f);
            body.torque.f[j] = mul(body.torque.f[j], 8.f);
        }
        for (int step = 0; step < 8; ++step) {
            VUState* outputs = step == 7 ? &vu : nullptr;
            integrate(body, outputs);
            wheel_contacts(body, records.wheels, records.tuning, outputs);
        }
        for (int j = 0; j < 3; ++j) {
            body.force.f[j] = mul(body.force.f[j], .125f);
            body.torque.f[j] = mul(body.torque.f[j], .125f);
        }
        std::memcpy(vu.mem, &records, sizeof(RigidBody) + sizeof(WheelSet));
        store(vu.vf[1], load(body.torque));
        store(vu.vf[2], load(body.force));
        vu.vi[4] = 0;
        vu.vi[5] = 46;
        vu.vi[6] = 32;
        vu.vi[7] = 36;
        vu.vi[8] = 19;
        vu.vi[9] = 24;
        vu.vi[10] = 28;
        vu.vi[11] = 0;
        vu.i = .125f;
        // Eight fixed body iterations, four wheels each. The last reciprocal is
        // ready 151 cycles before the return, and all writes have retired.
        vu.cycle += 12011;
        vu.q_ready = vu.cycle - 151;
        vu.tpc = 0xcf8;
        VUReg travelDelta{}, angleDelta{};
        for (int lane = 0; lane < 4; ++lane) {
            travelDelta.f[lane] = mul(records.wheels.travelRate.f[lane], dt);
            angleDelta.f[lane] = mul(records.wheels.spinRate.f[lane], dt);
        }
        const uint32_t flags[] = {mac(travelDelta, 15),
                                  mac(angleDelta, 15),
                                  mac(records.wheels.travel, 15),
                                  mac(records.wheels.angle, 15),
                                  0,
                                  0,
                                  mac(vu.vf[2], 14),
                                  mac(vu.vf[1], 14)};
        constexpr unsigned readyBeforeEnd[] = {30, 29, 26, 25, 7, 6, 3, 2};
        for (unsigned i = 0; i < 8; ++i)
            vu.fq[(vu.fq_head + i) & 7] = {vu.cycle - readyBeforeEnd[i], flags[i], 0};
        vu.mac = flags[7];
        // Every path computes zero and a negative constant. Divider inputs are
        // lengths or positive lower-bounded slip denominators; only RSQRT can
        // encounter zero. The final reciprocal clears the current D/I flags.
        vu.status = (vu.status & ~0x3fu) | 0xc0u | (subnormal ? 0x100 : 0) | (divideZero ? 0x800 : 0) |
                    (vu.mac & 15 ? 1 : 0) | (vu.mac & 240 ? 2 : 0) | (vu.mac & 0xf00 ? 4 : 0) |
                    (vu.mac & 0xf000 ? 8 : 0);
    }
};
} // namespace
void vehicle_dynamics_step(VUState& state) { Solver{}.run(state); }

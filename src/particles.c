#include "particles.h"

#include "log.h"
#include "math_compat.h"
#include "renderer.h"
#include "runner.h"
#include "utils.h"

#include "stb_ds.h"

#define PARTICLE_DEG2RAD (M_PI / 180.0)

// Particles draw from their own random stream instead of rand(). Sharing rand() would make every
// particle spawn shift the sequence the game itself sees, so merely adding a particle effect to a
// scene would change unrelated randomised behaviour (and every seeded screenshot test with it).
// The trade-off is that --seed and randomize() do not reach particles.
#define PARTICLE_RNG_SEED 0x9E3779B9u
static uint32_t g_particleRngState = PARTICLE_RNG_SEED;

static uint32_t particleRandomBits(void) {
    uint32_t x = g_particleRngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_particleRngState = x;
    return x;
}

// Uniform in [0, 1).
static GMLReal particleRandom01(void) {
    return (GMLReal) (particleRandomBits() >> 8) / (GMLReal) 0x01000000u;
}

// Uniform between the two bounds. Deliberately NOT normalised to (min <= max): GameMaker computes
// "min + random * (max - min)" and lets a reversed pair sweep downward, which games rely on.
// DELTARUNE Chapter 5 calls part_type_direction with (-1, -165), (-45, -90) and (-40, -90); each is
// a fan that collapses into a single direction the moment the ends are clamped.
//
// Worth spelling out because the HTML5 runtime disagrees: its MyRandom() returns the low bound
// whenever (max - min) <= 0, so those three sprays would come out as straight jets there. The games
// this runner is pointed at are built against the Windows runtime, so that is the one to match.
static GMLReal particleRandomRange(GMLReal min, GMLReal max) {
    return min + particleRandom01() * (max - min);
}

// Triangle wave in [-1, 1] driven by the particle's age. Each wiggling property runs on its own
// period and its own multiple of the particle's seed, so the four oscillations never line up and a
// spray of particles does not pulse in unison. The periods below are GameMaker's.
static GMLReal particleWiggle(int32_t age, int32_t seed, int32_t seedMultiplier, int32_t period) {
    GMLReal t = (GMLReal) (4 * ((age + seedMultiplier * seed) % period)) / (GMLReal) period; // 0..4
    if (t > 2.0) t = 4.0 - t;                                                                // fold to 0..2
    return t - 1.0;
}

static GMLReal particleWiggleDirection(int32_t age, int32_t seed) { return particleWiggle(age, seed, 3, 24); }
static GMLReal particleWiggleSpeed(int32_t age, int32_t seed)     { return particleWiggle(age, seed, 4, 20); }
static GMLReal particleWiggleAngle(int32_t age, int32_t seed)     { return particleWiggle(age, seed, 2, 16); }
static GMLReal particleWiggleSize(int32_t age, int32_t seed)      { return particleWiggle(age, seed, 1, 16); }

// "number" follows the GML convention shared by part_emitter_stream and part_type_death: a positive
// value is a literal count, a negative value is a 1-in-|number| chance of spawning a single particle.
// Emitters take a real count, and GameMaker spends the fraction as a chance of one extra particle,
// so part_emitter_stream(..., 0.5) emits on roughly every other step.
static int32_t particleResolveCount(GMLReal number) {
    if (0.0 > number) return (particleRandom01() * -number < 1.0) ? 1 : 0;

    int32_t whole = (int32_t) number;
    GMLReal fraction = number - (GMLReal) whole;
    if (fraction > 0.0 && particleRandom01() <= fraction) whole++;
    return whole;
}

// Uniform integer in [min, max] with both ends included, which is how GameMaker reads part_type_life.
// Truncating a real drawn from [min, max) would never return max. Reversed pairs sweep downward for
// the same reason particleRandomRange() lets them.
static int32_t particleRandomIntRange(int32_t min, int32_t max) {
    GMLReal span = (GMLReal) max - (GMLReal) min;
    span += (span >= 0.0) ? 1.0 : -1.0;
    return min + (int32_t) (particleRandom01() * span);
}

// ===[ Pools ]===

ParticleSystem* Particles_systemGet(Runner* runner, int32_t systemId) {
    if (0 > systemId || systemId >= (int32_t) arrlen(runner->particleSystemPool)) return nullptr;
    ParticleSystem* system = &runner->particleSystemPool[systemId];
    return system->used ? system : nullptr;
}

ParticleType* Particles_typeGet(Runner* runner, int32_t typeId) {
    if (0 > typeId || typeId >= (int32_t) arrlen(runner->particleTypePool)) return nullptr;
    ParticleType* type = &runner->particleTypePool[typeId];
    return type->used ? type : nullptr;
}

int32_t Particles_systemCreate(Runner* runner) {
    int32_t poolSize = (int32_t) arrlen(runner->particleSystemPool);
    int32_t id = poolSize;
    repeat(poolSize, i) {
        if (!runner->particleSystemPool[i].used) { id = (int32_t) i; break; }
    }

    ParticleSystem system;
    ZERO_STRUCT(system);
    system.used = true;
    system.automaticUpdate = true;
    system.automaticDraw = true;
    system.depth = 0;

    if (id == poolSize) {
        arrput(runner->particleSystemPool, system);
    } else {
        runner->particleSystemPool[id] = system;
    }

    // The system joins the depth-sorted draw list while automaticDraw is set.
    runner->drawableListStructureDirty = true;
    return id;
}

void Particles_systemDestroy(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;

    arrfree(system->particles);
    arrfree(system->emitters);
    ZERO_STRUCT(*system);
    runner->drawableListStructureDirty = true;
}

void Particles_systemSetDepth(Runner* runner, int32_t systemId, int32_t depth) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr || system->depth == depth) return;
    system->depth = depth;
    runner->drawableListSortDirty = true;
}

void Particles_systemSetAutomaticDraw(Runner* runner, int32_t systemId, bool automatic) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr || system->automaticDraw == automatic) return;
    system->automaticDraw = automatic;
    // Only switching drawing ON has to rebuild, since that is what adds an entry the cache does not
    // hold. Switching it off is filtered at draw time, like instance visibility, so the common
    // "automatic_draw(false) then drawit()" idiom re-issued every frame does not drag a full rebuild
    // and re-sort of every drawable in the room behind it.
    if (automatic) runner->drawableListStructureDirty = true;
}

void Particles_systemClear(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;

    arrsetlen(system->particles, 0);
    arrsetlen(system->emitters, 0);
    system->automaticUpdate = true;
    system->automaticDraw = true;
    system->depth = 0;
    system->originX = 0.0;
    system->originY = 0.0;
    system->warnedFull = false;
    // Depth and automatic drawing both just moved, so the cached list has to be rebuilt either way.
    runner->drawableListStructureDirty = true;
}

void Particles_systemClearParticles(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;
    arrsetlen(system->particles, 0);
}

int32_t Particles_systemParticleCount(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    return (system == nullptr) ? 0 : (int32_t) arrlen(system->particles);
}

// GameMaker's defaults for a fresh type: one white unit-sized particle, no motion, 100 steps.
static void particleTypeSetDefaults(ParticleType* type) {
    ZERO_STRUCT(*type);
    type->used = true;
    type->sprite = -1;
    type->sizeMin = 1.0;
    type->sizeMax = 1.0;
    type->scaleX = 1.0;
    type->scaleY = 1.0;
    type->lifeMin = 100;
    type->lifeMax = 100;
    type->alphaStart = 1.0;
    type->alphaMiddle = 1.0;
    type->alphaEnd = 1.0;
    type->colourStart = 0xFFFFFFu;
    type->colourMiddle = 0xFFFFFFu;
    type->colourEnd = 0xFFFFFFu;
    type->deathType = -1;
    type->stepType = -1;
}

int32_t Particles_typeCreate(Runner* runner) {
    int32_t poolSize = (int32_t) arrlen(runner->particleTypePool);
    int32_t id = poolSize;
    repeat(poolSize, i) {
        if (!runner->particleTypePool[i].used) { id = (int32_t) i; break; }
    }

    ParticleType type;
    particleTypeSetDefaults(&type);

    if (id == poolSize) {
        arrput(runner->particleTypePool, type);
    } else {
        runner->particleTypePool[id] = type;
    }
    return id;
}

void Particles_typeClear(Runner* runner, int32_t typeId) {
    ParticleType* type = Particles_typeGet(runner, typeId);
    if (type == nullptr) return;
    particleTypeSetDefaults(type);
}

void Particles_typeDestroy(Runner* runner, int32_t typeId) {
    ParticleType* type = Particles_typeGet(runner, typeId);
    if (type == nullptr) return;
    ZERO_STRUCT(*type);
    // Particles already alive keep their typeId. Drawing and stepping both resolve the type every
    // frame and skip when it is gone, so a destroyed type simply stops its remaining particles.
}

int32_t Particles_emitterCreate(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return -1;

    int32_t count = (int32_t) arrlen(system->emitters);
    int32_t id = count;
    repeat(count, i) {
        if (!system->emitters[i].used) { id = (int32_t) i; break; }
    }

    ParticleEmitter emitter;
    ZERO_STRUCT(emitter);
    emitter.used = true;
    emitter.shape = PS_SHAPE_RECTANGLE;
    emitter.distribution = PS_DISTR_LINEAR;
    emitter.streamType = -1;

    if (id == count) {
        arrput(system->emitters, emitter);
    } else {
        system->emitters[id] = emitter;
    }
    return id;
}

ParticleEmitter* Particles_emitterGet(Runner* runner, int32_t systemId, int32_t emitterId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return nullptr;
    if (0 > emitterId || emitterId >= (int32_t) arrlen(system->emitters)) return nullptr;
    ParticleEmitter* emitter = &system->emitters[emitterId];
    return emitter->used ? emitter : nullptr;
}

void Particles_emitterDestroy(Runner* runner, int32_t systemId, int32_t emitterId) {
    ParticleEmitter* emitter = Particles_emitterGet(runner, systemId, emitterId);
    if (emitter == nullptr) return;
    ZERO_STRUCT(*emitter);
}

void Particles_emitterDestroyAll(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;
    arrsetlen(system->emitters, 0);
}

// ===[ Spawning ]===

// Returns false once the system is full, so the callers' loops can stop instead of spinning through a
// spawn count that came straight from GML and may be enormous.
static bool particleSpawnAt(Runner* runner, ParticleSystem* system, int32_t typeId, GMLReal x, GMLReal y, uint32_t colour, bool fixedColour) {
    ParticleType* type = Particles_typeGet(runner, typeId);
    if (type == nullptr) return false;

    if ((int32_t) arrlen(system->particles) >= PARTICLE_SYSTEM_MAX_PARTICLES) {
        if (!system->warnedFull) {
            system->warnedFull = true;
            logWarn("Particles: system hit the %d particle cap, further spawns are dropped\n", PARTICLE_SYSTEM_MAX_PARTICLES);
        }
        return false;
    }

    Particle particle;
    ZERO_STRUCT(particle);
    particle.typeId = typeId;
    particle.x = x;
    particle.y = y;
    particle.speed = particleRandomRange(type->speedMin, type->speedMax);
    particle.direction = particleRandomRange(type->dirMin, type->dirMax);
    particle.size = particleRandomRange(type->sizeMin, type->sizeMax);
    particle.angle = particleRandomRange(type->angMin, type->angMax);
    particle.colour = colour;
    particle.colourFixed = fixedColour;
    particle.lifeTotal = particleRandomIntRange(type->lifeMin, type->lifeMax);
    if (1 > particle.lifeTotal) particle.lifeTotal = 1;
    particle.life = particle.lifeTotal;
    particle.seed = (int32_t) (particleRandomBits() % 100000u);

    if (type->spriteRandom && type->sprite >= 0 && runner->dataWin != nullptr && (uint32_t) type->sprite < runner->dataWin->sprt.count) {
        uint32_t frames = runner->dataWin->sprt.sprites[type->sprite].textureCount;
        if (frames > 0) particle.subimgBase = (int32_t) (particleRandomBits() % frames);
    }

    arrput(system->particles, particle);
    return true;
}

// Picks a point inside the emitter's region. Only the linear distribution is modelled; the gaussian
// ones fall back to it (no game we test against uses them, and guessing at the curve would be worse
// than an honest uniform spread).
static void particleEmitterPoint(ParticleEmitter* emitter, GMLReal* outX, GMLReal* outY) {
    GMLReal centerX = (emitter->xmin + emitter->xmax) * 0.5;
    GMLReal centerY = (emitter->ymin + emitter->ymax) * 0.5;
    GMLReal halfW = (emitter->xmax - emitter->xmin) * 0.5;
    GMLReal halfH = (emitter->ymax - emitter->ymin) * 0.5;

    // Every shape is sampled directly rather than by rejection. Beyond being cheaper and branch-free,
    // it cannot emit outside the region: a bounded rejection loop has to accept whatever it holds when
    // the attempts run out, which for a diamond (half the bounding box) leaks a particle into a corner
    // often enough to see.
    if (emitter->shape == PS_SHAPE_ELLIPSE) {
        // sqrt on the radius keeps the spread uniform by area instead of clustering at the centre.
        GMLReal radius = GMLReal_sqrt(particleRandom01());
        GMLReal angle = particleRandom01() * 2.0 * M_PI;
        *outX = centerX + halfW * radius * GMLReal_cos(angle);
        *outY = centerY + halfH * radius * GMLReal_sin(angle);
        return;
    }

    if (emitter->shape == PS_SHAPE_DIAMOND) {
        // Fold the unit square onto the triangle u + v <= 1, then mirror into a random quadrant.
        GMLReal u = particleRandom01();
        GMLReal v = particleRandom01();
        if (u + v > 1.0) { u = 1.0 - u; v = 1.0 - v; }
        uint32_t quadrant = particleRandomBits();
        if (quadrant & 1u) u = -u;
        if (quadrant & 2u) v = -v;
        *outX = centerX + halfW * u;
        *outY = centerY + halfH * v;
        return;
    }

    if (emitter->shape == PS_SHAPE_LINE) {
        // A line from (xmin, ymin) to (xmax, ymax), not the rectangle they bound.
        GMLReal t = particleRandom01();
        *outX = emitter->xmin + (emitter->xmax - emitter->xmin) * t;
        *outY = emitter->ymin + (emitter->ymax - emitter->ymin) * t;
        return;
    }

    *outX = particleRandomRange(emitter->xmin, emitter->xmax);
    *outY = particleRandomRange(emitter->ymin, emitter->ymax);
}

static void particleEmitterSpawn(Runner* runner, ParticleSystem* system, ParticleEmitter* emitter, int32_t typeId, int32_t count) {
    repeat(count, i) {
        GMLReal x, y;
        particleEmitterPoint(emitter, &x, &y);
        if (!particleSpawnAt(runner, system, typeId, x, y, 0xFFFFFFu, false)) return;
    }
}

void Particles_emitterBurst(Runner* runner, int32_t systemId, int32_t emitterId, int32_t typeId, GMLReal number) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    ParticleEmitter* emitter = Particles_emitterGet(runner, systemId, emitterId);
    if (system == nullptr || emitter == nullptr) return;
    particleEmitterSpawn(runner, system, emitter, typeId, particleResolveCount(number));
}

void Particles_particlesCreate(Runner* runner, int32_t systemId, GMLReal x, GMLReal y, int32_t typeId, int32_t number, uint32_t colour, bool fixedColour) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;
    repeat(number, i) {
        if (!particleSpawnAt(runner, system, typeId, x, y, colour, fixedColour)) return;
    }
}

// ===[ Update ]===

void Particles_updateSystem(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;

    // GameMaker ages a system, then moves it, and only then lets the emitters spawn into it. The
    // order is worth preserving: it is what makes a streamed particle sit still for the step it is
    // born on, while a death particle -- which joins between the two passes -- travels immediately.

    // Collected rather than spawned in place, because arrput() can move the array out from under the
    // loop walking it. Only allocated when a type actually asks for step or death particles.
    typedef struct { int32_t typeId; GMLReal x, y; int32_t count; } PendingSpawn;
    PendingSpawn* pending = nullptr;

    // Pass 1: age everything, note the spawns each particle's type asks for, bury what ran out.
    int32_t index = 0;
    while (index < (int32_t) arrlen(system->particles)) {
        Particle* particle = &system->particles[index];
        ParticleType* type = Particles_typeGet(runner, particle->typeId);

        if (type == nullptr) {
            // The type was destroyed underneath us; drop the particle instead of stepping a dead one.
            system->particles[index] = arrlast(system->particles);
            arrpop(system->particles);
            continue;
        }

        particle->life--;
        bool died = (0 >= particle->life);

        // A particle spawns its type's step particles every step it survives, and its death particles
        // instead of them on the step it does not. The two never fire on the same step.
        int32_t spawnType = died ? type->deathType : type->stepType;
        int32_t spawnNumber = died ? type->deathNumber : type->stepNumber;
        if (spawnType >= 0 && spawnNumber != 0) {
            int32_t count = particleResolveCount((GMLReal) spawnNumber);
            if (count > 0) {
                PendingSpawn spawn;
                spawn.typeId = spawnType;
                spawn.x = particle->x;
                spawn.y = particle->y;
                spawn.count = count;
                arrput(pending, spawn);
            }
        }

        if (!died) {
            index++;
            continue;
        }

        // Swap-remove: order within a system does not affect the drawn result, every particle of a
        // system is drawn in the same pass at the same depth.
        system->particles[index] = arrlast(system->particles);
        arrpop(system->particles);
    }

    // The collected spawns land before the movement pass, so they move on the step they are born.
    repeat((int32_t) arrlen(pending), i) {
        repeat(pending[i].count, n) {
            if (!particleSpawnAt(runner, system, pending[i].typeId, pending[i].x, pending[i].y, 0xFFFFFFu, false)) break;
        }
    }
    arrfree(pending);

    // Pass 2: increments, then gravity, then the move. The increment lands before the particle
    // travels, so its very first step already runs at (speed + speed increment).
    int32_t particleCount = (int32_t) arrlen(system->particles);
    repeat(particleCount, i) {
        Particle* particle = &system->particles[i];
        ParticleType* type = Particles_typeGet(runner, particle->typeId);
        if (type == nullptr) continue;

        particle->speed += type->speedIncr;
        if (0.0 > particle->speed) particle->speed = 0.0; // GameMaker never lets a particle reverse
        particle->direction += type->dirIncr;
        particle->angle += type->angIncr;

        if (type->gravityAmount != 0.0) {
            // Gravity folds into the velocity vector permanently, so later speed/direction increments
            // apply on top of it. Matches GameMaker, where gravity bends a particle's course for good.
            GMLReal baseRadians = particle->direction * PARTICLE_DEG2RAD;
            GMLReal gravityRadians = type->gravityDirection * PARTICLE_DEG2RAD;
            GMLReal hspeed = particle->speed * GMLReal_cos(baseRadians) + type->gravityAmount * GMLReal_cos(gravityRadians);
            GMLReal vspeed = -particle->speed * GMLReal_sin(baseRadians) - type->gravityAmount * GMLReal_sin(gravityRadians);
            particle->speed = GMLReal_sqrt(hspeed * hspeed + vspeed * vspeed);
            if (hspeed != 0.0 || vspeed != 0.0)
                particle->direction = GMLReal_atan2(-vspeed, hspeed) / PARTICLE_DEG2RAD;
        }

        int32_t age = particle->lifeTotal - particle->life;
        GMLReal effectiveSpeed = particle->speed + type->speedWiggle * particleWiggleSpeed(age, particle->seed);
        GMLReal effectiveDirection = particle->direction + type->dirWiggle * particleWiggleDirection(age, particle->seed);

        GMLReal radians = effectiveDirection * PARTICLE_DEG2RAD;
        particle->x += effectiveSpeed * GMLReal_cos(radians);
        particle->y -= effectiveSpeed * GMLReal_sin(radians); // GML's y axis grows downward

        // GameMaker's third pass, less the colour and alpha curves: those are functions of the
        // particle's age alone, so they are evaluated at draw time rather than stored per particle.
        particle->size += type->sizeIncr;
        if (0.0 > particle->size) particle->size = 0.0;
    }

    // Emitters stream last, into a system where everything already alive has finished moving.
    int32_t emitterCount = (int32_t) arrlen(system->emitters);
    repeat(emitterCount, i) {
        ParticleEmitter* emitter = &system->emitters[i];
        if (!emitter->used || 0 > emitter->streamType || emitter->streamNumber == 0.0) continue;
        particleEmitterSpawn(runner, system, emitter, emitter->streamType, particleResolveCount(emitter->streamNumber));
    }
}

void Particles_updateAutomatic(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->particleSystemPool);
    repeat(count, i) {
        ParticleSystem* system = &runner->particleSystemPool[i];
        if (!system->used || !system->automaticUpdate) continue;
        Particles_updateSystem(runner, (int32_t) i);
    }
}

// ===[ Draw ]===

// Alpha follows the three stop points across the particle's life: start -> middle at the halfway
// mark -> end. part_type_alpha1/alpha2 are expressed by collapsing the stops onto each other.
static GMLReal particleAlphaAt(const ParticleType* type, GMLReal ageFraction) {
    if (0.5 > ageFraction) {
        GMLReal t = ageFraction * 2.0;
        return type->alphaStart + (type->alphaMiddle - type->alphaStart) * t;
    }
    GMLReal t = (ageFraction - 0.5) * 2.0;
    return type->alphaMiddle + (type->alphaEnd - type->alphaMiddle) * t;
}

// Same three stops as the alpha curve. Interpolated per byte, which is correct whatever order the
// channels sit in: GML colours are passed straight through to the renderer without repacking.
static uint32_t particleColourLerp(uint32_t from, uint32_t to, GMLReal t) {
    uint32_t out = 0;
    repeat(3, shift) {
        int32_t bits = (int32_t) shift * 8;
        GMLReal a = (GMLReal) ((from >> bits) & 0xFFu);
        GMLReal b = (GMLReal) ((to >> bits) & 0xFFu);
        int32_t v = (int32_t) (a + (b - a) * t + 0.5);
        if (0 > v) v = 0;
        if (v > 255) v = 255;
        out |= ((uint32_t) v) << bits;
    }
    return out;
}

uint32_t Particles_colourMidpoint(uint32_t from, uint32_t to) {
    return particleColourLerp(from, to, 0.5);
}

static uint32_t particleColourAt(const ParticleType* type, GMLReal ageFraction) {
    if (0.5 > ageFraction)
        return particleColourLerp(type->colourStart, type->colourMiddle, ageFraction * 2.0);
    return particleColourLerp(type->colourMiddle, type->colourEnd, (ageFraction - 0.5) * 2.0);
}

void Particles_drawSystem(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr || runner->renderer == nullptr) return;

    int32_t count = (int32_t) arrlen(system->particles);
    if (count == 0) return;

    Renderer* renderer = runner->renderer;
    bool additiveActive = false;
    // Blend state is global and sticky in GML, so an additive type has to hand back exactly what the
    // caller had rather than assuming bm_normal: a game that darkens the scene with
    // gpu_set_blendmode_ext around its draw would otherwise lose the effect from the particle system
    // onwards. Captured lazily, so a system of ordinary particles never touches blending at all.
    bool blendTouched = false;
    bool blendSaved = false;
    int32_t savedBlendMode = bm_normal;
    BlendFactors savedBlendFactors;
    ZERO_STRUCT(savedBlendFactors);

    repeat(count, i) {
        Particle* particle = &system->particles[i];
        ParticleType* type = Particles_typeGet(runner, particle->typeId);
        if (type == nullptr || 0 > type->sprite) continue;

        int32_t age = particle->lifeTotal - particle->life;
        GMLReal ageFraction = (GMLReal) age / (GMLReal) particle->lifeTotal;
        GMLReal alpha = particleAlphaAt(type, ageFraction);
        if (0.0 >= alpha) continue;
        if (alpha > 1.0) alpha = 1.0;

        GMLReal size = particle->size + type->sizeWiggle * particleWiggleSize(age, particle->seed);
        if (0.0 >= size) continue;

        int32_t subimg = particle->subimgBase;
        if (type->spriteAnimate) {
            if (type->spriteStretch) {
                // One full animation cycle stretched over the particle's whole life.
                uint32_t frames = ((uint32_t) type->sprite < runner->dataWin->sprt.count)
                                ? runner->dataWin->sprt.sprites[type->sprite].textureCount : 0;
                if (frames > 0) subimg += (int32_t) (ageFraction * (GMLReal) frames);
            } else {
                subimg += age;
            }
        }

        if (type->additive != additiveActive) {
            if (!blendTouched) {
                // The getters are optional in the vtable; without them the best we can do is put
                // blending back to bm_normal at the end.
                if (renderer->vtable->gpuGetBlendMode != nullptr) {
                    savedBlendMode = renderer->vtable->gpuGetBlendMode(renderer);
                    if (renderer->vtable->gpuGetBlendFactors != nullptr)
                        savedBlendFactors = renderer->vtable->gpuGetBlendFactors(renderer);
                    blendSaved = true;
                }
                blendTouched = true;
            }
            renderer->vtable->gpuSetBlendMode(renderer, type->additive ? bm_add : bm_normal);
            additiveActive = type->additive;
        }

        uint32_t colour = particle->colourFixed ? particle->colour : particleColourAt(type, ageFraction);

        // A relative orientation is measured from the direction the particle is travelling, so a
        // sprite drawn nose-first keeps pointing along its arc as gravity bends it.
        GMLReal angle = particle->angle + type->angWiggle * particleWiggleAngle(age, particle->seed);
        if (type->angRelative) angle += particle->direction;

        Renderer_drawSpriteExt(renderer, type->sprite, subimg,
                               (float) (system->originX + particle->x), (float) (system->originY + particle->y),
                               (float) (type->scaleX * size), (float) (type->scaleY * size),
                               (float) angle, colour, (float) alpha);
    }

    if (!blendTouched) return;

    if (!blendSaved) {
        renderer->vtable->gpuSetBlendMode(renderer, bm_normal);
    } else if (savedBlendMode == bm_complex && renderer->vtable->gpuSetBlendModeExt != nullptr) {
        // gpu_set_blendmode_ext leaves the mode reading back as bm_complex, so the individual
        // factors are the only faithful way to put that state back.
        renderer->vtable->gpuSetBlendModeExt(renderer, savedBlendFactors.src, savedBlendFactors.dst,
                                             savedBlendFactors.srcAlpha, savedBlendFactors.dstAlpha);
    } else {
        renderer->vtable->gpuSetBlendMode(renderer, savedBlendMode);
    }
}

// ===[ Teardown ]===

void Particles_freeAll(Runner* runner) {
    int32_t count = (int32_t) arrlen(runner->particleSystemPool);
    repeat(count, i) {
        arrfree(runner->particleSystemPool[i].particles);
        arrfree(runner->particleSystemPool[i].emitters);
    }
    arrfree(runner->particleSystemPool);
    runner->particleSystemPool = nullptr;
    arrfree(runner->particleTypePool);
    runner->particleTypePool = nullptr;
    // Reached on game_restart as well as shutdown, so put the stream back to where it started:
    // a restarted game should produce the same particles as the first run.
    g_particleRngState = PARTICLE_RNG_SEED;
}

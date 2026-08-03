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
static uint32_t g_particleRngState = 0x9E3779B9u;

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
// "min + random * (max - min)", and games depend on the reversed form. part_type_direction(-45, -90)
// in DELTARUNE Chapter 4 sweeps downward from -45, and swapping the bounds would flip the spray.
static GMLReal particleRandomRange(GMLReal min, GMLReal max) {
    return min + particleRandom01() * (max - min);
}

// Triangle wave in [-1, 1] driven by the particle's phase counter. GameMaker does not document its
// wiggle period; this approximates the oscillation without a sin() per property per particle per frame.
static GMLReal particleWiggle(uint8_t phase) {
    GMLReal t = (GMLReal) phase / 128.0; // 0..2
    return (1.0 > t) ? (t * 2.0 - 1.0) : (3.0 - t * 2.0);
}

// "number" follows the GML convention shared by part_emitter_stream and part_type_death: a positive
// value is a literal count, a negative value is a 1-in-|number| chance of spawning a single particle.
static int32_t particleResolveCount(int32_t number) {
    if (number >= 0) return number;
    int32_t chance = -number;
    return ((int32_t) (particleRandomBits() % (uint32_t) chance) == 0) ? 1 : 0;
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
    // Entering or leaving the depth list changes the SET of drawables, not just their order.
    runner->drawableListStructureDirty = true;
}

int32_t Particles_typeCreate(Runner* runner) {
    int32_t poolSize = (int32_t) arrlen(runner->particleTypePool);
    int32_t id = poolSize;
    repeat(poolSize, i) {
        if (!runner->particleTypePool[i].used) { id = (int32_t) i; break; }
    }

    // GameMaker's defaults for a fresh type: a single white pixel-sized particle, no motion, 100 steps.
    ParticleType type;
    ZERO_STRUCT(type);
    type.used = true;
    type.sprite = -1;
    type.sizeMin = 1.0;
    type.sizeMax = 1.0;
    type.scaleX = 1.0;
    type.scaleY = 1.0;
    type.lifeMin = 100;
    type.lifeMax = 100;
    type.alphaStart = 1.0;
    type.alphaMiddle = 1.0;
    type.alphaEnd = 1.0;
    type.deathType = -1;

    if (id == poolSize) {
        arrput(runner->particleTypePool, type);
    } else {
        runner->particleTypePool[id] = type;
    }
    return id;
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

static void particleSpawnAt(Runner* runner, ParticleSystem* system, int32_t typeId, GMLReal x, GMLReal y) {
    ParticleType* type = Particles_typeGet(runner, typeId);
    if (type == nullptr) return;

    if ((int32_t) arrlen(system->particles) >= PARTICLE_SYSTEM_MAX_PARTICLES) {
        if (!system->warnedFull) {
            system->warnedFull = true;
            logWarn("Particles: system hit the %d particle cap, further spawns are dropped\n", PARTICLE_SYSTEM_MAX_PARTICLES);
        }
        return;
    }

    Particle particle;
    ZERO_STRUCT(particle);
    particle.typeId = typeId;
    particle.x = x;
    particle.y = y;
    particle.speed = particleRandomRange(type->speedMin, type->speedMax);
    particle.direction = particleRandomRange(type->dirMin, type->dirMax);
    particle.size = particleRandomRange(type->sizeMin, type->sizeMax);
    particle.lifeTotal = (int32_t) particleRandomRange((GMLReal) type->lifeMin, (GMLReal) type->lifeMax);
    if (1 > particle.lifeTotal) particle.lifeTotal = 1;
    particle.life = particle.lifeTotal;
    particle.phase = (uint8_t) (particleRandomBits() & 0xFFu);

    if (type->spriteRandom && type->sprite >= 0 && runner->dataWin != nullptr && (uint32_t) type->sprite < runner->dataWin->sprt.count) {
        uint32_t frames = runner->dataWin->sprt.sprites[type->sprite].textureCount;
        if (frames > 0) particle.subimgBase = (int32_t) (particleRandomBits() % frames);
    }

    arrput(system->particles, particle);
}

// Picks a point inside the emitter's region. Only the linear distribution is modelled; the gaussian
// ones fall back to it (no game we test against uses them, and guessing at the curve would be worse
// than an honest uniform spread).
static void particleEmitterPoint(ParticleEmitter* emitter, GMLReal* outX, GMLReal* outY) {
    GMLReal x = particleRandomRange(emitter->xmin, emitter->xmax);
    GMLReal y = particleRandomRange(emitter->ymin, emitter->ymax);

    GMLReal centerX = (emitter->xmin + emitter->xmax) * 0.5;
    GMLReal centerY = (emitter->ymin + emitter->ymax) * 0.5;
    GMLReal halfW = (emitter->xmax - emitter->xmin) * 0.5;
    GMLReal halfH = (emitter->ymax - emitter->ymin) * 0.5;

    if (emitter->shape == PS_SHAPE_ELLIPSE || emitter->shape == PS_SHAPE_DIAMOND) {
        // Rejection sampling keeps the spread uniform. The regions are small and the acceptance rate
        // is 0.79 (ellipse) / 0.5 (diamond), so the loop is bounded in practice; cap it anyway.
        repeat(8, attempt) {
            GMLReal nx = (halfW > 0.0) ? (x - centerX) / halfW : 0.0;
            GMLReal ny = (halfH > 0.0) ? (y - centerY) / halfH : 0.0;
            bool inside = (emitter->shape == PS_SHAPE_ELLIPSE)
                        ? (nx * nx + ny * ny <= 1.0)
                        : (GMLReal_fabs(nx) + GMLReal_fabs(ny) <= 1.0);
            if (inside) break;
            x = particleRandomRange(emitter->xmin, emitter->xmax);
            y = particleRandomRange(emitter->ymin, emitter->ymax);
        }
    } else if (emitter->shape == PS_SHAPE_LINE) {
        // A line from (xmin, ymin) to (xmax, ymax), not the rectangle they bound.
        GMLReal t = particleRandom01();
        x = emitter->xmin + (emitter->xmax - emitter->xmin) * t;
        y = emitter->ymin + (emitter->ymax - emitter->ymin) * t;
    }

    *outX = x;
    *outY = y;
}

static void particleEmitterSpawn(Runner* runner, ParticleSystem* system, ParticleEmitter* emitter, int32_t typeId, int32_t count) {
    repeat(count, i) {
        GMLReal x, y;
        particleEmitterPoint(emitter, &x, &y);
        particleSpawnAt(runner, system, typeId, x, y);
    }
}

void Particles_emitterBurst(Runner* runner, int32_t systemId, int32_t emitterId, int32_t typeId, int32_t number) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    ParticleEmitter* emitter = Particles_emitterGet(runner, systemId, emitterId);
    if (system == nullptr || emitter == nullptr) return;
    particleEmitterSpawn(runner, system, emitter, typeId, particleResolveCount(number));
}

// ===[ Update ]===

void Particles_updateSystem(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr) return;

    // Emitters stream first, so a particle spawned this step also moves this step (as in GameMaker).
    int32_t emitterCount = (int32_t) arrlen(system->emitters);
    repeat(emitterCount, i) {
        ParticleEmitter* emitter = &system->emitters[i];
        if (!emitter->used || 0 > emitter->streamType || emitter->streamNumber == 0) continue;
        particleEmitterSpawn(runner, system, emitter, emitter->streamType, particleResolveCount(emitter->streamNumber));
    }

    // Deaths are collected and spawned after the movement pass: spawning mid-loop can realloc the
    // array out from under the iteration, and a death particle must not be stepped on its spawn frame.
    // Only allocated when a type actually has a death type, which is rare.
    typedef struct { int32_t typeId; GMLReal x, y; int32_t count; } PendingDeath;
    PendingDeath* deaths = nullptr;

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

        GMLReal wiggle = particleWiggle(particle->phase);
        GMLReal effectiveSpeed = particle->speed + type->speedWiggle * wiggle;
        GMLReal effectiveDirection = particle->direction + type->dirWiggle * wiggle;

        GMLReal radians = effectiveDirection * PARTICLE_DEG2RAD;
        particle->x += effectiveSpeed * GMLReal_cos(radians);
        particle->y -= effectiveSpeed * GMLReal_sin(radians); // GML's y axis grows downward

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

        particle->speed += type->speedIncr;
        if (0.0 > particle->speed) particle->speed = 0.0; // GameMaker never lets a particle reverse
        particle->direction += type->dirIncr;
        particle->size += type->sizeIncr;
        if (0.0 > particle->size) particle->size = 0.0;

        particle->phase = (uint8_t) ((particle->phase + 8u) & 0xFFu);
        particle->life--;

        if (particle->life > 0) {
            index++;
            continue;
        }

        if (type->deathType >= 0 && type->deathNumber != 0) {
            int32_t count = particleResolveCount(type->deathNumber);
            if (count > 0) {
                PendingDeath death;
                death.typeId = type->deathType;
                death.x = particle->x;
                death.y = particle->y;
                death.count = count;
                arrput(deaths, death);
            }
        }

        // Swap-remove: order within a system does not affect the drawn result, every particle of a
        // system is drawn in the same pass at the same depth.
        system->particles[index] = arrlast(system->particles);
        arrpop(system->particles);
    }

    repeat((int32_t) arrlen(deaths), i) {
        repeat(deaths[i].count, n) {
            particleSpawnAt(runner, system, deaths[i].typeId, deaths[i].x, deaths[i].y);
        }
    }
    arrfree(deaths);
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

void Particles_drawSystem(Runner* runner, int32_t systemId) {
    ParticleSystem* system = Particles_systemGet(runner, systemId);
    if (system == nullptr || runner->renderer == nullptr) return;

    int32_t count = (int32_t) arrlen(system->particles);
    if (count == 0) return;

    Renderer* renderer = runner->renderer;
    bool blendChanged = false;
    bool additiveActive = false;

    repeat(count, i) {
        Particle* particle = &system->particles[i];
        ParticleType* type = Particles_typeGet(runner, particle->typeId);
        if (type == nullptr || 0 > type->sprite) continue;

        GMLReal ageFraction = 1.0 - ((GMLReal) particle->life / (GMLReal) particle->lifeTotal);
        GMLReal alpha = particleAlphaAt(type, ageFraction);
        if (0.0 >= alpha) continue;
        if (alpha > 1.0) alpha = 1.0;

        GMLReal size = particle->size + type->sizeWiggle * particleWiggle(particle->phase);
        if (0.0 >= size) continue;

        int32_t subimg = particle->subimgBase;
        if (type->spriteAnimate) {
            if (type->spriteStretch) {
                // One full animation cycle stretched over the particle's whole life.
                uint32_t frames = ((uint32_t) type->sprite < runner->dataWin->sprt.count)
                                ? runner->dataWin->sprt.sprites[type->sprite].textureCount : 0;
                if (frames > 0) subimg += (int32_t) (ageFraction * (GMLReal) frames);
            } else {
                subimg += particle->lifeTotal - particle->life;
            }
        }

        // Only touched when an additive type is actually present, so a system of ordinary particles
        // leaves whatever blend mode the caller had set alone. There is no way to read the current
        // mode back out of the renderer, so once we do touch it the restore below can only go to
        // bm_normal, which is the mode GameMaker itself leaves behind after drawing a system.
        if (type->additive != additiveActive) {
            renderer->vtable->gpuSetBlendMode(renderer, type->additive ? bm_add : bm_normal);
            additiveActive = type->additive;
            blendChanged = true;
        }

        Renderer_drawSpriteExt(renderer, type->sprite, subimg,
                               (float) particle->x, (float) particle->y,
                               (float) (type->scaleX * size), (float) (type->scaleY * size),
                               0.0f, 0xFFFFFFu, (float) alpha);
    }

    if (blendChanged && additiveActive)
        renderer->vtable->gpuSetBlendMode(renderer, bm_normal);
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
}

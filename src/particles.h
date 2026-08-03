#ifndef _BS_PARTICLES_H_
#define _BS_PARTICLES_H_

#include "common.h"
#include "real_type.h"
#include <stdint.h>

// Forward declarations
#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif

// ===[ Particle System ]===
// GameMaker splits particles into three resources:
//   * a SYSTEM owns the live particles and the emitters that spawn them, and decides when they are drawn
//   * a TYPE describes how a particle looks and moves; types are global, so any system can stream any type
//   * an EMITTER is owned by one system and spawns particles of a given type inside a region
//
// Ids are indices into pools hanging off the Runner, with a "used" tombstone so a destroyed id can be
// handed out again. Same convention as the ds_* pools in vm_builtins.c, and games do rely on it.

// part_emitter_region() shape constants
#define PS_SHAPE_RECTANGLE 0
#define PS_SHAPE_ELLIPSE   1
#define PS_SHAPE_DIAMOND   2
#define PS_SHAPE_LINE      3

// part_emitter_region() distribution constants
#define PS_DISTR_LINEAR    0
#define PS_DISTR_GAUSSIAN  1
#define PS_DISTR_INVGAUSS  2

// Upper bound on live particles per system. GameMaker itself has no such limit, but an emitter left
// streaming in a room the player never leaves will grow without bound, and the consoles this runner
// targets cannot absorb that. Spawns past the cap are dropped (warned about once per system).
#define PARTICLE_SYSTEM_MAX_PARTICLES 8192

typedef struct {
    bool used;

    int32_t sprite;       // sprite asset index, -1 when the type has no sprite (draws nothing)
    bool spriteAnimate;   // advance the subimage as the particle ages
    bool spriteStretch;   // stretch one full animation cycle across the particle's whole life
    bool spriteRandom;    // start from a random subimage

    // Every "min/max/incr/wiggle" quadruple works the same way: the initial value is picked uniformly
    // in [min, max], "incr" is added every step, and "wiggle" oscillates the value used for motion and
    // drawing without accumulating into the base.
    GMLReal sizeMin, sizeMax, sizeIncr, sizeWiggle;
    GMLReal scaleX, scaleY;
    GMLReal speedMin, speedMax, speedIncr, speedWiggle;
    GMLReal dirMin, dirMax, dirIncr, dirWiggle;

    GMLReal gravityAmount;
    GMLReal gravityDirection;

    int32_t lifeMin, lifeMax;

    GMLReal alphaStart, alphaMiddle, alphaEnd;
    bool additive;

    int32_t deathType;   // type id spawned when a particle of this type dies, -1 when none
    int32_t deathNumber; // how many to spawn; negative means a 1-in-|n| chance
} ParticleType;

typedef struct {
    int32_t typeId;
    GMLReal x, y;
    GMLReal speed;      // base speed, before wiggle
    GMLReal direction;  // base direction in degrees, before wiggle
    GMLReal size;       // base size, before wiggle
    int32_t life;       // steps remaining
    int32_t lifeTotal;  // steps this particle started with, for the alpha/animation curves
    int32_t subimgBase; // starting subimage
    uint8_t phase;      // wiggle phase, advanced every step
} Particle;

typedef struct {
    bool used;
    GMLReal xmin, xmax, ymin, ymax;
    int32_t shape;
    int32_t distribution;
    int32_t streamType;   // type id streamed every step, -1 when the emitter is idle
    int32_t streamNumber; // particles per step; negative means a 1-in-|n| chance
} ParticleEmitter;

typedef struct {
    bool used;
    bool automaticUpdate; // step the system at the end of every frame (on by default, as in GML)
    bool automaticDraw;   // draw the system from the depth list (on by default, as in GML)
    int32_t depth;
    bool warnedFull;      // the "hit PARTICLE_SYSTEM_MAX_PARTICLES" warning fires once per system
    Particle* particles;  // stb_ds array
    ParticleEmitter* emitters; // stb_ds array, index = emitter id within this system
} ParticleSystem;

// ===[ Systems ]===
int32_t Particles_systemCreate(Runner* runner);
void Particles_systemDestroy(Runner* runner, int32_t systemId);
ParticleSystem* Particles_systemGet(Runner* runner, int32_t systemId);
void Particles_systemSetDepth(Runner* runner, int32_t systemId, int32_t depth);
void Particles_systemSetAutomaticDraw(Runner* runner, int32_t systemId, bool automatic);

// ===[ Types ]===
int32_t Particles_typeCreate(Runner* runner);
void Particles_typeDestroy(Runner* runner, int32_t typeId);
ParticleType* Particles_typeGet(Runner* runner, int32_t typeId);

// ===[ Emitters ]===
int32_t Particles_emitterCreate(Runner* runner, int32_t systemId);
ParticleEmitter* Particles_emitterGet(Runner* runner, int32_t systemId, int32_t emitterId);
void Particles_emitterDestroy(Runner* runner, int32_t systemId, int32_t emitterId);
void Particles_emitterDestroyAll(Runner* runner, int32_t systemId);
void Particles_emitterBurst(Runner* runner, int32_t systemId, int32_t emitterId, int32_t typeId, int32_t number);

// ===[ Frame hooks ]===
// Steps one system: emitters stream, particles move and age, dead particles run their death spawn.
void Particles_updateSystem(Runner* runner, int32_t systemId);
// Steps every system with automaticUpdate set. Called once at the end of Runner_step.
void Particles_updateAutomatic(Runner* runner);
// Draws one system at the current draw state. Backs part_system_drawit and the depth-list entry.
void Particles_drawSystem(Runner* runner, int32_t systemId);

// Frees both pools. Called from the Runner's cleanup path.
void Particles_freeAll(Runner* runner);

#endif /* _BS_PARTICLES_H_ */

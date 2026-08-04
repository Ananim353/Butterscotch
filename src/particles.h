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

    // Drawn orientation. Independent of the direction of travel unless angRelative is set, in which
    // case the angle is measured from it.
    GMLReal angMin, angMax, angIncr, angWiggle;
    bool angRelative;

    int32_t lifeMin, lifeMax;

    // Alpha and colour both run through three stops across the particle's life: start, middle at the
    // halfway mark, end. part_type_alpha1/alpha2 and part_type_colour1/colour2 collapse the stops.
    GMLReal alphaStart, alphaMiddle, alphaEnd;
    uint32_t colourStart, colourMiddle, colourEnd; // GML packed BGR, as the drawing functions take it
    bool additive;

    int32_t deathType;   // type id spawned when a particle of this type dies, -1 when none
    int32_t deathNumber; // how many to spawn; negative means a 1-in-|n| chance
    int32_t stepType;    // type id spawned every step a particle of this type lives, -1 when none
    int32_t stepNumber;  // same "negative means a chance" convention as deathNumber
} ParticleType;

typedef struct {
    int32_t typeId;
    GMLReal x, y;
    GMLReal speed;      // base speed, before wiggle
    GMLReal direction;  // base direction in degrees, before wiggle
    GMLReal size;       // base size, before wiggle
    GMLReal angle;      // drawn orientation in degrees, before wiggle
    int32_t life;       // steps remaining
    int32_t lifeTotal;  // steps this particle started with, for the alpha/animation curves
    int32_t subimgBase; // starting subimage
    uint32_t colour;    // set by part_particles_create_colour; overrides the type's colour curve
    bool colourFixed;   // true when "colour" above is in force
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
    GMLReal originX, originY;  // part_system_position: added to every particle when drawing
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
// Resets the system to how part_system_create left it: no particles, no emitters, depth 0, both
// automatic flags back on.
void Particles_systemClear(Runner* runner, int32_t systemId);
// Removes every live particle but leaves the emitters and settings in place.
void Particles_systemClearParticles(Runner* runner, int32_t systemId);
int32_t Particles_systemParticleCount(Runner* runner, int32_t systemId);
// Spawns particles directly, bypassing emitters. "colour" is honoured only when fixedColour is set,
// which is what separates part_particles_create_colour from part_particles_create.
void Particles_particlesCreate(Runner* runner, int32_t systemId, GMLReal x, GMLReal y, int32_t typeId, int32_t number, uint32_t colour, bool fixedColour);

// ===[ Types ]===
int32_t Particles_typeCreate(Runner* runner);
void Particles_typeDestroy(Runner* runner, int32_t typeId);
// Puts a live type back to the defaults a freshly created one has.
void Particles_typeClear(Runner* runner, int32_t typeId);
ParticleType* Particles_typeGet(Runner* runner, int32_t typeId);
// Blend of two GML colours, used by part_type_colour2 to place the middle stop of a two-stop curve
// on the straight line between its ends.
uint32_t Particles_colourMidpoint(uint32_t from, uint32_t to);

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

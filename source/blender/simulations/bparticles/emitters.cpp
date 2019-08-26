#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_curve_types.h"
#include "DNA_object_types.h"

#include "BKE_curve.h"
#include "BKE_mesh_runtime.h"

#include "BLI_math_geom.h"

#include "FN_types.hpp"

#include "emitters.hpp"

namespace BParticles {

using FN::SharedList;
using namespace FN::Types;

static float random_float()
{
  return (rand() % 4096) / 4096.0f;
}

void PointEmitter::emit(EmitterInterface &interface)
{
  uint amount = 10;
  Vector<float3> new_positions(amount);
  Vector<float3> new_velocities(amount);
  Vector<float> new_sizes(amount);
  Vector<float> birth_times(amount);

  for (uint i = 0; i < amount; i++) {
    float t = i / (float)amount;
    new_positions[i] = m_position.interpolate(t);
    new_velocities[i] = m_velocity.interpolate(t);
    new_sizes[i] = m_size.interpolate(t);
    birth_times[i] = interface.time_span().interpolate(t);
  }

  for (StringRef type : m_types_to_emit) {
    auto new_particles = interface.particle_allocator().request(type, new_positions.size());
    new_particles.set<float3>("Position", new_positions);
    new_particles.set<float3>("Velocity", new_velocities);
    new_particles.set<float>("Size", new_sizes);
    new_particles.set<float>("Birth Time", birth_times);
  }
}

static float3 random_point_in_triangle(float3 a, float3 b, float3 c)
{
  float3 dir1 = b - a;
  float3 dir2 = c - a;
  float rand1, rand2;

  do {
    rand1 = random_float();
    rand2 = random_float();
  } while (rand1 + rand2 > 1.0f);

  return a + dir1 * rand1 + dir2 * rand2;
}

void SurfaceEmitter::emit(EmitterInterface &interface)
{
  if (m_object == nullptr) {
    return;
  }
  if (m_object->type != OB_MESH) {
    return;
  }

  float particles_to_emit_f = m_rate * interface.time_span().duration();
  float fraction = particles_to_emit_f - std::floor(particles_to_emit_f);
  if ((rand() % 1000) / 1000.0f < fraction) {
    particles_to_emit_f = std::floor(particles_to_emit_f) + 1;
  }
  uint particles_to_emit = particles_to_emit_f;

  Mesh *mesh = (Mesh *)m_object->data;

  MLoop *loops = mesh->mloop;
  MVert *verts = mesh->mvert;
  const MLoopTri *triangles = BKE_mesh_runtime_looptri_ensure(mesh);
  int triangle_amount = BKE_mesh_runtime_looptri_len(mesh);
  if (triangle_amount == 0) {
    return;
  }

  Vector<float3> positions;
  Vector<float3> velocities;
  Vector<float> sizes;
  Vector<float> birth_times;

  for (uint i = 0; i < particles_to_emit; i++) {
    MLoopTri triangle = triangles[rand() % triangle_amount];
    float birth_moment = random_float();

    float3 v1 = verts[loops[triangle.tri[0]].v].co;
    float3 v2 = verts[loops[triangle.tri[1]].v].co;
    float3 v3 = verts[loops[triangle.tri[2]].v].co;
    float3 pos = random_point_in_triangle(v1, v2, v3);

    float3 normal;
    normal_tri_v3(normal, v1, v2, v3);

    float epsilon = 0.01f;
    float4x4 transform_at_birth = m_transform.interpolate(birth_moment);
    float4x4 transform_before_birth = m_transform.interpolate(birth_moment - epsilon);

    float3 point_at_birth = transform_at_birth.transform_position(pos);
    float3 point_before_birth = transform_before_birth.transform_position(pos);

    float3 normal_velocity = transform_at_birth.transform_direction(normal);
    float3 emitter_velocity = (point_at_birth - point_before_birth) / epsilon;

    positions.append(point_at_birth);
    velocities.append(normal_velocity * m_normal_velocity + emitter_velocity * m_emitter_velocity);
    birth_times.append(interface.time_span().interpolate(birth_moment));
    sizes.append(m_size);
  }

  for (StringRef type_name : m_types_to_emit) {
    auto new_particles = interface.particle_allocator().request(type_name, positions.size());
    new_particles.set<float3>("Position", positions);
    new_particles.set<float3>("Velocity", velocities);
    new_particles.set<float>("Size", sizes);
    new_particles.set<float>("Birth Time", birth_times);

    m_on_birth_action->execute_from_emitter(new_particles, interface);
  }
}

void InitialGridEmitter::emit(EmitterInterface &interface)
{
  if (!interface.is_first_step()) {
    return;
  }

  Vector<float3> new_positions;

  float offset_x = -(m_amount_x * m_step_x / 2.0f);
  float offset_y = -(m_amount_y * m_step_y / 2.0f);

  for (uint x = 0; x < m_amount_x; x++) {
    for (uint y = 0; y < m_amount_y; y++) {
      new_positions.append(float3(x * m_step_x + offset_x, y * m_step_y + offset_y, 0.0f));
    }
  }

  for (StringRef type_name : m_types_to_emit) {
    auto new_particles = interface.particle_allocator().request(type_name, new_positions.size());
    new_particles.set<float3>("Position", new_positions);
    new_particles.fill<float>("Birth Time", interface.time_span().start());
    new_particles.fill<float>("Size", m_size);
  }
}

}  // namespace BParticles

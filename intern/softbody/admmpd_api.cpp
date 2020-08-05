/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2013 Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup admmpd
 */

#include "admmpd_api.h"
#include "admmpd_types.h"
#include "admmpd_solver.h"
#include "admmpd_mesh.h"
#include "admmpd_collision.h"

#include "tetgen_api.h"
#include "DNA_mesh_types.h" // Mesh
#include "DNA_meshdata_types.h" // MVert
#include "DNA_object_force_types.h" // Enums
#include "BKE_mesh_remesh_voxel.h" // TetGen
#include "BKE_mesh.h" // BKE_mesh_free
#include "BKE_softbody.h" // BodyPoint
#include "MEM_guardedalloc.h"

#include <iostream>
#include <memory>
#include <algorithm>

#define ADMMPD_API_DEBUG

struct ADMMPDInternalData
{
  // init_mesh:
  std::unique_ptr<admmpd::Collision> collision;
  std::shared_ptr<admmpd::Mesh> mesh; // collision stores a ptr
  // init_solver:
  std::unique_ptr<admmpd::Options> options;
  std::unique_ptr<admmpd::SolverData> data;
  std::unique_ptr<admmpd::Solver> solver;
  int substeps;
};


static inline void strcpy_error(ADMMPDInterfaceData *iface, const std::string &str)
{
  int len = std::min(256, (int)str.size()+1);
  memset(iface->last_error, 0, sizeof(iface->last_error));
  str.copy(iface->last_error, len);
}

static inline void options_from_object(
  Object *ob,
  admmpd::Options *op,
  bool &reset_mesh,
  bool &reset_solver)
{
  reset_mesh = false;
  reset_solver = false;

  SoftBody *sb = ob->soft;
  if (sb==NULL)
    return;

  // Set options that don't require a re-initialization
  op->max_admm_iters = std::max(1,sb->admmpd_max_admm_iters);
  op->min_res = std::max(0.f,sb->admmpd_converge_eps);
  op->mult_pk = std::max(0.f,std::min(1.f,sb->admmpd_goalstiff));
  op->mult_ck = std::max(0.f,std::min(1.f,sb->admmpd_collisionstiff));
  op->floor = sb->admmpd_floor_z;
  op->self_collision = sb->admmpd_self_collision;
  op->log_level = std::max(0, std::min(LOGLEVEL_NUM-1, sb->admmpd_loglevel));
  op->grav = Eigen::Vector3d(0,0,sb->admmpd_gravity);

  const double diffeps = 1e-10;

  // Options that cause considerable change in
  // precomupted variables:
  if (std::abs(op->density_kgm3 - sb->admmpd_density_kgm3)>diffeps) {
    op->density_kgm3 = std::max(1.f,sb->admmpd_density_kgm3);
    reset_solver = true;
  }
  double new_youngs = std::pow(10.f, std::max(0.f,sb->admmpd_youngs_exp));
  if (std::abs(op->youngs - new_youngs)>diffeps) {
    op->density_kgm3 = new_youngs;
    reset_solver = true;
  }
  if (std::abs(op->poisson - sb->admmpd_poisson)>diffeps) {
    op->poisson = std::max(0.f,std::min(0.499f,sb->admmpd_poisson));
    reset_solver = true;
  }
  if (std::abs(op->poisson - sb->admmpd_poisson)>diffeps) {
    op->poisson = std::max(0.f,std::min(0.499f,sb->admmpd_poisson));
    reset_solver = true;
  }
  if (op->linsolver != sb->admmpd_linsolver) {
    op->linsolver = std::max(0, std::min(LINSOLVER_NUM-1, sb->admmpd_linsolver));
    reset_solver = true;
  }
  if (op->elastic_material != sb->admmpd_material) {
    op->elastic_material = std::max(0, std::min(ELASTIC_NUM-1, sb->admmpd_material));
    reset_solver = true;
  }
}

static inline void vecs_from_object(
  Object *ob,
  float (*vertexCos)[3],
  std::vector<float> &v,
  std::vector<unsigned int> &f)
{
  if(ob->type != OB_MESH)
    return;

  Mesh *me = (Mesh*)ob->data;

  // Initialize input vertices
  v.resize(me->totvert*3, 0);
  for (int i=0; i<me->totvert; ++i)
  {
    // Local to global coordinates
    float vi[3];
    vi[0] = vertexCos[i][0];
    vi[1] = vertexCos[i][1];
    vi[2] = vertexCos[i][2];
    mul_m4_v3(ob->obmat, vi);
    for (int j=0; j<3; ++j) {
      v[i*3+j] = vi[j];
    }
  } // end loop input surface verts

  // Initialize input faces
  int totfaces = poly_to_tri_count(me->totpoly, me->totloop);
  f.resize(totfaces*3, 0);
  MLoopTri *looptri, *lt;
  looptri = lt = (MLoopTri *)MEM_callocN(sizeof(*looptri)*totfaces, __func__);
  BKE_mesh_recalc_looptri(me->mloop, me->mpoly, me->mvert, me->totloop, me->totpoly, looptri);
  for (int i=0; i<totfaces; ++i, ++lt)
  {
      f[i*3+0] = me->mloop[lt->tri[0]].v;
      f[i*3+1] = me->mloop[lt->tri[1]].v;
      f[i*3+2] = me->mloop[lt->tri[2]].v;
  }
  MEM_freeN(looptri);
  looptri = NULL;
}

void admmpd_dealloc(ADMMPDInterfaceData *iface)
{
  if (iface==NULL)
    return;

  // Do not change mesh_totverts or
  // mesh_totfaces, because those are input and
  // admmpd_dealloc is called on init.

  iface->out_totverts = 0; // output vertices
  memset(iface->last_error, 0, sizeof(iface->last_error));
  if (iface->idata)
  {
    iface->idata->options.reset();
    iface->idata->data.reset();
    iface->idata->solver.reset();
    iface->idata->collision.reset();
    iface->idata->mesh.reset();
  }

  iface->idata = nullptr;
}

static inline int admmpd_init_with_tetgen(ADMMPDInterfaceData *iface, Object *ob, float (*vertexCos)[3])
{
  std::vector<float> v;
  std::vector<unsigned int> f;
  vecs_from_object(ob,vertexCos,v,f);

  TetGenRemeshData tg;
  init_tetgenremeshdata(&tg);
  tg.in_verts = v.data();
  tg.in_totverts = v.size()/3;
  tg.in_faces = f.data();
  tg.in_totfaces = f.size()/3;
  bool success = tetgen_resmesh(&tg);
  if (!success || tg.out_tottets==0)
  {
    strcpy_error(iface, "TetGen failed to generate");
    return 0;
  }

  // Double check assumption, the first
  // mesh_totverts vertices remain the same
  // for input and output mesh.
  #ifdef ADMMPD_API_DEBUG
    for (int i=0; i<tg.in_totverts; ++i)
    {
      for (int j=0; j<3; ++j)
      {
        float diff = std::abs(v[i*3+j]-tg.out_verts[i*3+j]);
        if (diff > 1e-10)
        {
          strcpy_error(iface, "Bad TetGen assumption: change in surface verts");
          return 0;
        }
      }
    }
  #endif

  iface->out_totverts = tg.out_totverts;
  iface->idata->mesh = std::make_shared<admmpd::TetMesh>();
  success = iface->idata->mesh->create(
    tg.out_verts,
    tg.out_totverts,
    tg.out_facets,
    tg.out_totfacets,
    tg.out_tets,
    tg.out_tottets);

  if (!success || iface->out_totverts==0)
  {
    strcpy_error(iface, "TetMesh failed on creation");
    return 0;
  }

  // Clean up tetgen output data
  MEM_freeN(tg.out_tets);
  MEM_freeN(tg.out_facets);
  MEM_freeN(tg.out_verts);

  return 1;
}

static inline int admmpd_init_with_lattice(ADMMPDInterfaceData *iface, Object *ob, float (*vertexCos)[3])
{
  std::vector<float> v;
  std::vector<unsigned int> f;
  vecs_from_object(ob,vertexCos,v,f);

  iface->out_totverts = 0;
  iface->idata->mesh = std::make_shared<admmpd::EmbeddedMesh>();
  std::shared_ptr<admmpd::EmbeddedMesh> emb_msh =
    std::dynamic_pointer_cast<admmpd::EmbeddedMesh>(iface->idata->mesh);
  emb_msh->options.max_subdiv_levels = ob->soft->admmpd_embed_res;
  bool success = iface->idata->mesh->create(
    v.data(),
    v.size()/3,
    f.data(),
    f.size()/3,
    nullptr,
    0);

  iface->out_totverts = iface->idata->mesh->rest_prim_verts()->rows();
  if (!success)
  {
    strcpy_error(iface, "EmbeddedMesh failed on creation");
    return 0;
  }

  iface->idata->collision = std::make_unique<admmpd::EmbeddedMeshCollision>(emb_msh);
  return 1;
}

static inline int admmpd_init_as_cloth(ADMMPDInterfaceData *iface, Object *ob, float (*vertexCos)[3])
{
  std::vector<float> v;
  std::vector<unsigned int> f;
  vecs_from_object(ob,vertexCos,v,f);

  iface->out_totverts = 0;
  iface->idata->mesh = std::make_shared<admmpd::TriangleMesh>();
  bool success = iface->idata->mesh->create(
    v.data(),
    v.size()/3,
    f.data(),
    f.size()/3,
    nullptr,
    0);

  iface->out_totverts = iface->idata->mesh->rest_facet_verts()->rows();
  if (!success)
  {
    strcpy_error(iface, "TriangleMesh failed on creation");
    return 0;
  }

  iface->idata->collision = nullptr; // TODO, triangle mesh collision
  return 1;
}

// Given the mesh, options, and data, initializes the solver
static inline int admmpd_reinit_solver(ADMMPDInterfaceData *iface)
{
  try
  {
    iface->idata->solver->init(
      iface->idata->mesh.get(),
      iface->idata->options.get(),
      iface->idata->data.get());
  }
  catch(const std::exception &e)
  {
    strcpy_error(iface, e.what());
    return 0;
  }
  return 1;
}


int admmpd_init(ADMMPDInterfaceData *iface, Object *ob, float (*vertexCos)[3], int mode)
{
  if (iface==NULL || ob==NULL)
  {
    strcpy_error(iface, "NULL input");
    return 0;
  }

  SoftBody *sb = ob->soft;
  if (sb==NULL)
  {
    strcpy_error(iface, "NULL SoftBody input");
    return 0;
  }

  // Delete any existing data
  admmpd_dealloc(iface);

  // Generate solver data if it doesn't exist
  iface->idata = new ADMMPDInternalData();
  iface->idata->substeps = std::max(1,sb->admmpd_substeps);
  iface->idata->solver = std::make_unique<admmpd::Solver>();
  iface->idata->options = std::make_unique<admmpd::Options>();
  iface->idata->data = std::make_unique<admmpd::SolverData>();
  float fps = std::min(1000.f,std::max(1.f,iface->in_framerate));
  admmpd::Options *op = iface->idata->options.get();
  op->timestep_s = (1.0/fps) / float(std::max(1,sb->admmpd_substeps));
  bool renew_mesh, renew_solver;
  options_from_object(ob,op,renew_mesh,renew_solver);

  // Initialize the mesh
  try
  {
    int gen_success = 0;
    switch (mode)
    {
      default:
      case ADMMPD_INIT_MODE_EMBEDDED: {
        gen_success = admmpd_init_with_lattice(iface,ob,vertexCos);
      } break;
      case ADMMPD_INIT_MODE_TETGEN: {
        gen_success = admmpd_init_with_tetgen(iface,ob,vertexCos);
      } break;
      case ADMMPD_INIT_MODE_TRIANGLE: {
        gen_success = admmpd_init_as_cloth(iface,ob,vertexCos);
      } break;
    }
    if (!gen_success || iface->out_totverts==0)
    {
      return 0;
    }
  }
  catch(const std::exception &e)
  {
    strcpy_error(iface, e.what());
    return 0;
  }

  // Initialize the solver
  if (!admmpd_reinit_solver(iface))
    return 0;

  return 1;
}


void admmpd_copy_from_bodypoint(ADMMPDInterfaceData *iface, const BodyPoint *pts)
{
  if (iface == NULL || pts == NULL)
    return;

  for (int i=0; i<iface->out_totverts; ++i)
  {
    const BodyPoint *pt = &pts[i];
    for(int j=0; j<3; ++j)
    {
      iface->idata->data->x(i,j)=pt->pos[j];
      iface->idata->data->v(i,j)=pt->vec[j];
    }
  }
}

void admmpd_update_obstacles(
    ADMMPDInterfaceData *iface,
    float *in_verts_0,
    float *in_verts_1,
    int nv,
    unsigned int *in_faces,
    int nf)
{
    if (iface==NULL || in_verts_0==NULL || in_verts_1==NULL || in_faces==NULL)
      return;
    if (!iface->idata)
      return;
    if (!iface->idata->collision)
      return;

    iface->idata->collision->set_obstacles(
      in_verts_0, in_verts_1, nv, in_faces, nf);
}

void admmpd_update_goals(
    ADMMPDInterfaceData *iface,
    float *goal_k, // goal stiffness, nv
    float *goal_pos, // goal position, nv x 3
    int nv)
{
    if (iface==NULL || goal_k==NULL || goal_pos==NULL)
      return;
    if (!iface->idata)
      return;
    if (!iface->idata->mesh)
      return;

    for (int i=0; i<nv; ++i)
    {
      // We want to call set_pin for every vertex, even
      // if stiffness is zero. This allows us to animate pins on/off
      // without calling Mesh::clear_pins().
      Eigen::Vector3d qi(goal_pos[i*3+0], goal_pos[i*3+1], goal_pos[i*3+2]);
      iface->idata->mesh->set_pin(i,qi,goal_k[i]);
    }
}

void admmpd_copy_to_bodypoint_and_object(ADMMPDInterfaceData *iface, BodyPoint *pts, float (*vertexCos)[3])
{

  if (iface == NULL)
    return;

  // Map the deforming vertices to BodyPoint
  for (int i=0; i<iface->out_totverts; ++i)
  {
    if (pts != NULL)
    {
      BodyPoint *pt = &pts[i];
      for(int j=0; j<3; ++j)
      {
        pt->pos[j] = iface->idata->data->x(i,j);
        pt->vec[j] = iface->idata->data->v(i,j);
      }
    }
  }

  // Map the facet vertices
  const Eigen::MatrixXd *rest_facet_verts = iface->idata->mesh->rest_facet_verts();
  if (vertexCos != NULL && rest_facet_verts != nullptr)
  {
    int num_surf_verts = rest_facet_verts->rows();
    for (int i=0; i<num_surf_verts; ++i)
    {
      Eigen::Vector3d xi =
        iface->idata->mesh->get_mapped_facet_vertex(
        &iface->idata->data->x, i);
      vertexCos[i][0] = xi[0];
      vertexCos[i][1] = xi[1];
      vertexCos[i][2] = xi[2];
    }
  }

} // end map ADMMPD to bodypoint and object

int admmpd_solve(ADMMPDInterfaceData *iface, Object *ob)
{
  
  if (iface==NULL || ob==NULL || ob->soft==NULL)
  {
    strcpy_error(iface, "NULL input");
    return 0;
  }

  if (!iface->idata || !iface->idata->options ||
    !iface->idata->data || !iface->idata->solver)
  {
    strcpy_error(iface, "NULL internal data");
    return 0;
  }

// TODO: Figure this out
//  bool renew_mesh = false;
//  bool renew_solver = false;
//  options_from_object(
//    ob,
//    iface->idata->options.get(),
//    renew_mesh,
//    renew_solver);
//  if (renew_solver)
//    admmpd_reinit_solver(iface);

  try
  {
    int substeps = std::max(1,iface->idata->substeps);
    for (int i=0; i<substeps; ++i)
    {
      iface->idata->solver->solve(
        iface->idata->mesh.get(),
        iface->idata->options.get(),
        iface->idata->data.get(),
        iface->idata->collision.get());
    }
  }
  catch(const std::exception &e)
  {
    iface->idata->data->x = iface->idata->data->x_start;
    strcpy_error(iface, e.what());
    return 0;
  }
  return 1;
}
/********************************************************************************
 * HOMMEXX 1.0: Copyright of Sandia Corporation
 * This software is released under the BSD license
 * See the file 'COPYRIGHT' in the HOMMEXX/src/share/cxx directory
 *******************************************************************************/

#include "HyperviscosityFunctorImpl.hpp"

#include "Context.hpp"
#include "FunctorsBuffersManager.hpp"
#include "profiling.hpp"

#include "mpi/BoundaryExchange.hpp"
#include "mpi/MpiBuffersManager.hpp"
#include "mpi/Connectivity.hpp"

namespace Homme
{

HyperviscosityFunctorImpl::
HyperviscosityFunctorImpl (const SimulationParams&       params,
                           const Elements&               elements)
 : m_data (params.hypervis_subcycle,params.nu_ratio1,params.nu_ratio2,params.nu_top,params.nu,params.nu_p,params.nu_s,params.hypervis_scaling)
 , m_state   (elements.m_state)
 , m_derived (elements.m_derived)
 , m_geometry (elements.m_geometry)
 , m_sphere_ops (Context::singleton().get<SphereOperators>())
 , m_hvcoord (Context::singleton().get<HybridVCoord>())
 , m_policy_ref_states (Homme::get_default_team_policy<ExecSpace,TagRefStates>(elements.num_elems()))
 , m_policy_update_states (0, elements.num_elems()*NP*NP*NUM_LEV)
 , m_policy_first_laplace (Homme::get_default_team_policy<ExecSpace,TagFirstLaplaceHV>(elements.num_elems()))
 , m_policy_pre_exchange (Homme::get_default_team_policy<ExecSpace, TagHyperPreExchange>(elements.num_elems()))
{
  // Sanity check
  assert(params.params_set);

  if (m_data.nu_top>0) {

    m_nu_scale_top = ExecViewManaged<Scalar[NUM_LEV]>("nu_scale_top");
    ExecViewManaged<Scalar[NUM_LEV]>::HostMirror h_nu_scale_top;
    h_nu_scale_top = Kokkos::create_mirror_view(m_nu_scale_top);

    Kokkos::Array<Real,NUM_BIHARMONIC_PHYSICAL_LEVELS> lev_nu_scale_top = { 4.0, 2.0, 1.0 };
    for (int phys_lev=0; phys_lev<NUM_BIHARMONIC_PHYSICAL_LEVELS; ++phys_lev) {
      const int ilev = phys_lev / VECTOR_SIZE;
      const int ivec = phys_lev % VECTOR_SIZE;
      h_nu_scale_top(ilev)[ivec] = lev_nu_scale_top[phys_lev]*m_data.nu_top;
    }
    Kokkos::deep_copy(m_nu_scale_top, h_nu_scale_top);
  }

  // Init ElementOps
  m_elem_ops.init(m_hvcoord);

  // Make sure the sphere operators have buffers large enough to accommodate this functor's needs
  m_sphere_ops.allocate_buffers(Homme::get_default_team_policy<ExecSpace>(m_state.num_elems()));
}

int HyperviscosityFunctorImpl::requested_buffer_size () const {
  constexpr int size_mid_scalar = NP*NP*NUM_LEV;
  constexpr int size_int_scalar = NP*NP*NUM_LEV_P;

  const int ne = m_geometry.num_elems();
  const int nteams = get_num_concurrent_teams(m_policy_pre_exchange); 

  return 3*ne*size_mid_scalar + 2*ne*size_int_scalar + nteams*NP*NP;
}

void HyperviscosityFunctorImpl::init_buffers (const FunctorsBuffersManager& fbm) {
  Errors::runtime_check(fbm.allocated_size()>=requested_buffer_size(), "Error! Buffers size not sufficient.\n");

  constexpr int size_mid_scalar =   NP*NP*NUM_LEV;
  constexpr int size_mid_vector = 2*NP*NP*NUM_LEV;
  constexpr int size_int_scalar =   NP*NP*NUM_LEV_P;

  Scalar* mem = reinterpret_cast<Scalar*>(fbm.get_memory());
  const int ne = m_geometry.num_elems();
  const int nteams = get_num_concurrent_teams(m_policy_pre_exchange); 

  // Midpoints fields
  m_buffers.dp_ref = decltype(m_buffers.dp_ref)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.p = decltype(m_buffers.p)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.theta_ref = decltype(m_buffers.theta_ref)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.dptens = decltype(m_buffers.dptens)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.ttens = decltype(m_buffers.ttens)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.vtens = decltype(m_buffers.vtens)(mem,ne);
  mem += size_mid_vector*ne;

  m_buffers.lapl_dp = decltype(m_buffers.lapl_dp)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.lapl_theta = decltype(m_buffers.lapl_theta)(mem,ne);
  mem += size_mid_scalar*ne;

  m_buffers.lapl_v = decltype(m_buffers.lapl_v)(mem,ne);
  mem += size_mid_vector*ne;

  // Interfaces fields
  m_buffers.p_i       = decltype(m_buffers.p_i)(mem,ne);
  mem += size_int_scalar*ne;

  m_buffers.phi_i_ref = decltype(m_buffers.phi_i_ref)(mem,ne);
  mem += size_int_scalar*ne;

  m_buffers.wtens = decltype(m_buffers.wtens)(mem,ne);
  mem += size_int_scalar*ne;

  m_buffers.phitens = decltype(m_buffers.phitens)(mem,ne);
  mem += size_int_scalar*ne;

  m_buffers.lapl_w = decltype(m_buffers.lapl_w)(mem,ne);
  mem += size_int_scalar*ne;

  m_buffers.lapl_phi = decltype(m_buffers.lapl_phi)(mem,ne);
  mem += size_int_scalar*ne;

  // ps_ref can alias anything (except dp_ref), since it's used to compute dp_ref, then tossed
  m_buffers.ps_ref = decltype(m_buffers.ps_ref)(reinterpret_cast<Real*>(m_buffers.p.data()),nteams);

}

void HyperviscosityFunctorImpl::init_boundary_exchanges () {
  m_be = std::make_shared<BoundaryExchange>();
  auto& be = *m_be;
  auto bm_exchange = Context::singleton().get<MpiBuffersManagerMap>()[MPI_EXCHANGE];
  if (!bm_exchange->is_connectivity_set()) {
    bm_exchange->set_connectivity(Context::singleton().get_ptr<Connectivity>());
  }
  be.set_buffers_manager(bm_exchange);
  be.set_num_fields(0, 0, 4, 2);
  be.register_field(m_buffers.dptens);
  be.register_field(m_buffers.ttens);
  be.register_field(m_buffers.wtens);
  be.register_field(m_buffers.phitens);
  be.register_field(m_buffers.vtens, 2, 0);
  be.registration_completed();
}

void HyperviscosityFunctorImpl::run (const int np1, const Real dt, const Real eta_ave_w)
{
  m_data.np1 = np1;
  m_data.dt = dt/m_data.hypervis_subcycle;
  m_data.eta_ave_w = eta_ave_w;

  Kokkos::parallel_for(m_policy_ref_states,*this);

  for (int icycle = 0; icycle < m_data.hypervis_subcycle; ++icycle) {
    GPTLstart("hvf-bhwk");
    biharmonic_wk_theta ();
    GPTLstop("hvf-bhwk");
    // dispatch parallel_for for first kernel
    Kokkos::parallel_for(m_policy_pre_exchange, *this);
    Kokkos::fence();

    // // Exchange
    // assert (m_be->is_registration_completed());
    // GPTLstart("hvf-bexch");
    // m_be->exchange();
    // GPTLstop("hvf-bexch");

    // // Update states
    // Kokkos::parallel_for(policy_update_states, *this);
    Kokkos::fence();
  }
}

void HyperviscosityFunctorImpl::biharmonic_wk_theta() const
{
  // For the first laplacian we use a differnt kernel, which uses directly the states
  // at timelevel np1 as inputs, and subtracts the reference states.
  // This way we avoid copying the states to *tens buffers.
  
  auto policy_first_laplace = Homme::get_default_team_policy<ExecSpace,TagFirstLaplaceHV>(m_geometry.num_elems());
  Kokkos::parallel_for(policy_first_laplace, *this);
  Kokkos::fence();

  // Exchange
  assert (m_be->is_registration_completed());
  GPTLstart("hvf-bexch");
  m_be->exchange(m_geometry.m_rspheremp);
  GPTLstop("hvf-bexch");

  // TODO: update m_data.nu_ratio if nu_div!=nu
  // Compute second laplacian, tensor or const hv
  if ( m_data.consthv ) {
    auto policy_second_laplace = Homme::get_default_team_policy<ExecSpace,TagSecondLaplaceConstHV>(m_geometry.num_elems());
    Kokkos::parallel_for(policy_second_laplace, *this);
  }else{
    auto policy_second_laplace = Homme::get_default_team_policy<ExecSpace,TagSecondLaplaceTensorHV>(m_geometry.num_elems());
    Kokkos::parallel_for(policy_second_laplace, *this);
  }
  Kokkos::fence();
}

} // namespace Homme
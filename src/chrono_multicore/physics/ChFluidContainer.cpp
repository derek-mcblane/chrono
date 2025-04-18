// =============================================================================
// PROJECT CHRONO - http://projectchrono.org
//
// Copyright (c) 2016 projectchrono.org
// All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file at the top level of the distribution and at
// http://projectchrono.org/license-chrono.txt.
//
// =============================================================================
// Authors: Hammad Mazhar
// =============================================================================

#include <algorithm>
#include <cmath>

#include "chrono_multicore/ChDataManager.h"

#include "chrono_multicore/physics/ChSystemMulticore.h"
#include "chrono_multicore/physics/Ch3DOFContainer.h"
#include "chrono_multicore/physics/ChFluidKernels.h"

#include "chrono_multicore/constraints/ChConstraintUtils.h"

#include "chrono/multicore_math/thrust.h"
#include "chrono/multicore_math/matrix.h"

namespace chrono {

ChFluidContainer::ChFluidContainer() {
    body_offset = 0;
    epsilon = 1e-3;
    tau = 4 * .001;
    rho = 1000;
    mass = 1;

    viscosity = 0;

    artificial_pressure = false;
    artificial_pressure_k = 0.01;
    artificial_pressure_dq = .2 * kernel_radius;
    artificial_pressure_n = 4;
    enable_viscosity = false;
    nu = .2;
    youngs_modulus = 1.4e5;
    hardening_coefficient = 10;
    lame_lambda = youngs_modulus * nu / ((1. + nu) * (1. - 2. * nu));
    lame_mu = youngs_modulus / (2. * (1. + nu));
    theta_s = 7.5e-3;
    theta_c = 2.5e-2;
    alpha_flip = .95;
}

void ChFluidContainer::AddBodies(const std::vector<real3>& positions, const std::vector<real3>& velocities) {
    custom_vector<real3>& pos_fluid = data_manager->host_data.pos_3dof;
    custom_vector<real3>& vel_fluid = data_manager->host_data.vel_3dof;

    pos_fluid.insert(pos_fluid.end(), positions.begin(), positions.end());
    vel_fluid.insert(vel_fluid.end(), velocities.begin(), velocities.end());
    // In case the number of velocities provided were not enough, resize to the number of fluid bodies
    vel_fluid.resize(pos_fluid.size());
    data_manager->num_fluid_bodies = (int)pos_fluid.size();
}
void ChFluidContainer::Update3DOF(double time) {
    num_fluid_bodies = data_manager->num_fluid_bodies;
    num_rigid_bodies = data_manager->num_rigid_bodies;
    num_shafts = data_manager->num_shafts;
    num_motors = data_manager->num_motors;
    real3 g_acc = data_manager->settings.gravity;
    real3 h_gravity = data_manager->settings.step_size * mass * g_acc;

    uint offset = num_rigid_bodies * 6 + num_shafts + num_motors;
#pragma omp parallel for
    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        data_manager->host_data.hf[offset + i * 3 + 0] = h_gravity.x;
        data_manager->host_data.hf[offset + i * 3 + 1] = h_gravity.y;
        data_manager->host_data.hf[offset + i * 3 + 2] = h_gravity.z;
    }
}

void ChFluidContainer::UpdatePosition(double time) {
    num_fluid_bodies = data_manager->num_fluid_bodies;
    num_rigid_bodies = data_manager->num_rigid_bodies;
    num_shafts = data_manager->num_shafts;
    num_motors = data_manager->num_motors;
    custom_vector<real3>& pos_fluid = data_manager->host_data.pos_3dof;
    custom_vector<real3>& vel_fluid = data_manager->host_data.vel_3dof;

    uint offset = num_rigid_bodies * 6 + num_shafts + num_motors;
#pragma omp parallel for
    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        real3 vel;
        int original_index = data_manager->cd_data->particle_indices_3dof[i];
        // these are sorted so we have to unsort them
        vel.x = data_manager->host_data.v[offset + i * 3 + 0];
        vel.y = data_manager->host_data.v[offset + i * 3 + 1];
        vel.z = data_manager->host_data.v[offset + i * 3 + 2];

        real speed = Length(vel);
        if (speed > max_velocity) {
            vel = vel * max_velocity / speed;
        }
        vel_fluid[original_index] = vel;
        pos_fluid[original_index] += vel * data_manager->settings.step_size;
    }
}

unsigned int ChFluidContainer::GetNumConstraints() {
    int num_fluid_fluid = data_manager->num_fluid_bodies;

    if (contact_mu == 0) {
        num_fluid_fluid += data_manager->cd_data->num_rigid_fluid_contacts;
    } else {
        num_fluid_fluid += data_manager->cd_data->num_rigid_fluid_contacts * 3;
    }

    if (enable_viscosity) {
        num_fluid_fluid += data_manager->num_fluid_bodies * 3;
    }

    // printf("ChFluidContainer::GetNumConstraints() %d\n", num_fluid_fluid);
    return num_fluid_fluid;
}

unsigned int ChFluidContainer::GetNumNonZeros() {
    int nnz_fluid_fluid = data_manager->num_fluid_bodies * 6 * ChNarrowphase::max_neighbors;

    if (contact_mu == 0) {
        nnz_fluid_fluid += 9 * data_manager->cd_data->num_rigid_fluid_contacts;
    } else {
        nnz_fluid_fluid += 9 * 3 * data_manager->cd_data->num_rigid_fluid_contacts;
    }

    if (enable_viscosity) {
        nnz_fluid_fluid += data_manager->num_fluid_bodies * 18 * ChNarrowphase::max_neighbors;
    }
    // printf("ChFluidContainer::GetNumNonZeros() %d\n", nnz_fluid_fluid);
    return nnz_fluid_fluid;
}

void ChFluidContainer::ComputeInvMass(int offset) {
    num_fluid_bodies = data_manager->num_fluid_bodies;
    CompressedMatrix<real>& M_inv = data_manager->host_data.M_inv;

    real inv_mass = 1.0 / mass;
    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        M_inv.append(offset + i * 3 + 0, offset + i * 3 + 0, inv_mass);
        M_inv.finalize(offset + i * 3 + 0);
        M_inv.append(offset + i * 3 + 1, offset + i * 3 + 1, inv_mass);
        M_inv.finalize(offset + i * 3 + 1);
        M_inv.append(offset + i * 3 + 2, offset + i * 3 + 2, inv_mass);
        M_inv.finalize(offset + i * 3 + 2);
    }
}
void ChFluidContainer::ComputeMass(int offset) {
    num_fluid_bodies = data_manager->num_fluid_bodies;
    CompressedMatrix<real>& M = data_manager->host_data.M;

    real fluid_mass = mass;
    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        M.append(offset + i * 3 + 0, offset + i * 3 + 0, fluid_mass);
        M.finalize(offset + i * 3 + 0);
        M.append(offset + i * 3 + 1, offset + i * 3 + 1, fluid_mass);
        M.finalize(offset + i * 3 + 1);
        M.append(offset + i * 3 + 2, offset + i * 3 + 2, fluid_mass);
        M.finalize(offset + i * 3 + 2);
    }
}
void ChFluidContainer::Setup3DOF(int start_constraint) {
    Ch3DOFContainer::Setup3DOF(start_constraint);

    start_boundary = start_constraint;

    if (contact_mu == 0) {
        start_density = start_constraint + num_rigid_fluid_contacts;
    } else {
        start_density = start_constraint + num_rigid_fluid_contacts * 3;
    }

    start_viscous = start_density + num_fluid_bodies;

    body_offset = num_rigid_bodies * 6 + num_shafts + num_motors;
}

void ChFluidContainer::Initialize() {}

void ChFluidContainer::Density_Fluid() {
    custom_vector<real3>& sorted_pos = data_manager->host_data.sorted_pos_3dof;
    real h = kernel_radius;
    real inv_density = 1.0 / rho;
    real mass_over_density = mass * inv_density;
    CompressedMatrix<real>& D_T = data_manager->host_data.D_T;

#pragma omp parallel for
    for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
        real dens = 0;
        real3 dcon_diag = real3(0.0);
        real3 pos_p = sorted_pos[body_a];
        ////int d_ind = 0;
        for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
            int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
            if (body_a == body_b) {
                dens += mass * CPOLY6 * H6;
                ////d_ind = i;
                continue;
            }
            real3 xij = pos_p - sorted_pos[body_b];
            real dist = Length(xij);

            dens += mass * KPOLY6;

            real3 kernel_xij = KGSPIKY * xij;
            real3 dcon_od = mass_over_density * kernel_xij;  // off diagonal
            dcon_diag -= dcon_od;                            // diagonal is sum
            // den_con_jac[body_a * ChNarrowphase::max_neighbors + i] = dcon_od;
            SetRow3Check(D_T, start_density + body_a, body_offset + body_b * 3, dcon_od);
        }
        ////den_con_jac[body_a * ChNarrowphase::max_neighbors + d_ind] = dcon_diag;
        SetRow3Check(D_T, start_density + body_a, body_offset + body_a * 3, dcon_diag);
        density[body_a] = dens;
    }
}
void ChFluidContainer::Normalize_Density_Fluid() {
    real h = kernel_radius;
    custom_vector<real3>& sorted_pos = data_manager->host_data.sorted_pos_3dof;

#pragma omp parallel for
    for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
        real dens = 0;
        real3 pos_p = sorted_pos[body_a];
        for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
            int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
            if (body_a == body_b) {
                dens += mass / density[body_b] * CPOLY6 * H6;
                continue;
            }
            real3 xij = pos_p - sorted_pos[body_b];
            real dist = Length(xij);
            dens += (mass / density[body_b]) * KPOLY6;
        }
        density[body_a] = density[body_a] / dens;
    }
}

void ChFluidContainer::Build_D() {
    CompressedMatrix<real>& D_T = data_manager->host_data.D_T;

    BuildRigidFluidBoundary(contact_mu, num_fluid_bodies, body_offset, start_boundary, data_manager);

    if (data_manager->cd_data->num_fluid_contacts > 0) {
        real h = kernel_radius;
        real eta = .01;

        // custom_vector<real3>& vel = data_manager->host_data.vel_3dof;
        custom_vector<real3>& sorted_pos = data_manager->host_data.sorted_pos_3dof;

        //=======COMPUTE DENSITY OF FLUID
        density.resize(num_fluid_bodies);
        Density_Fluid();
        Normalize_Density_Fluid();

        real visca = viscosity;
        real viscb = viscosity;
        const real mass_2 = mass * mass;
        const real eta_2 = eta * eta;
        if (enable_viscosity) {
#pragma omp parallel for
            for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
                real3 pos_p = sorted_pos[body_a];
                real3 vmat_row1(0);
                real3 vmat_row2(0);
                real3 vmat_row3(0);
                for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
                    int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
                    if (body_a == body_b) {
                        continue;
                    }
                    real3 xij = pos_p - sorted_pos[body_b];
                    real dist = Length(xij);
                    real3 kernel_xij = KGSPIKY * xij;
                    //
                    real density_a = density[body_a];
                    real density_b = density[body_b];
                    real part_a = (8.0 / (density_a + density_b));
                    real part_b = (visca + viscb);
                    real part_c = 1.0 / (h * ((dist * dist / (H2)) + eta_2));
                    real scalar = -mass_2 * part_a * part_b * part_c;

                    real3 r1 = xij[0] * kernel_xij * scalar;
                    real3 r2 = xij[1] * kernel_xij * scalar;
                    real3 r3 = xij[2] * kernel_xij * scalar;

                    SetRow3Check(D_T, start_viscous + body_a * 3 + 0, body_offset + body_b * 3, r1);
                    SetRow3Check(D_T, start_viscous + body_a * 3 + 1, body_offset + body_b * 3, r2);
                    SetRow3Check(D_T, start_viscous + body_a * 3 + 2, body_offset + body_b * 3, r3);

                    vmat_row1 -= r1;
                    vmat_row2 -= r2;
                    vmat_row3 -= r3;
                }

                SetRow3Check(D_T, start_viscous + body_a * 3 + 0, body_offset + body_a * 3, vmat_row1);
                SetRow3Check(D_T, start_viscous + body_a * 3 + 1, body_offset + body_a * 3, vmat_row2);
                SetRow3Check(D_T, start_viscous + body_a * 3 + 2, body_offset + body_a * 3, vmat_row3);
            }
        }
    }
}
void ChFluidContainer::Build_b() {
    DynamicVector<real>& b = data_manager->host_data.b;

    CorrectionRigidFluidBoundary(contact_mu, contact_cohesion, alpha, contact_recovery_speed, num_fluid_bodies,
                                 start_boundary, data_manager);

    if (num_fluid_bodies > 0) {
#pragma omp parallel for
        for (int index = 0; index < (signed)num_fluid_bodies; index++) {
            b[start_density + index] = -(density[index] / rho - 1.0);
        }
    }
}
void ChFluidContainer::Build_E() {
    DynamicVector<real>& E = data_manager->host_data.E;
    ComplianceRigidFluidBoundary(contact_mu, contact_compliance, alpha, start_boundary, data_manager);

    real step_size = data_manager->settings.step_size;
    real zeta = 1.0 / (1.0 + 4.0 * tau / step_size);
    real f_compliance = 4.0 / (step_size * step_size) * (epsilon * zeta);

    if (num_fluid_bodies > 0) {
#pragma omp parallel for
        for (int index = 0; index < (signed)num_fluid_bodies; index++) {
            E[start_density + index] = f_compliance;
            if (enable_viscosity) {
                E[start_viscous + index * 3 + 0] = 0;
                E[start_viscous + index * 3 + 1] = 0;
                E[start_viscous + index * 3 + 2] = 0;
            }
        }
    }
}

void ChFluidContainer::Project(real* gamma) {
    ProjectRigidFluidBoundary(contact_mu, contact_cohesion, num_fluid_bodies, start_boundary, gamma, data_manager);
}

void ChFluidContainer::GenerateSparsity() {
    CompressedMatrix<real>& D_T = data_manager->host_data.D_T;
    AppendRigidFluidBoundary(contact_mu, num_fluid_bodies, body_offset, start_boundary, data_manager);

    if (data_manager->cd_data->num_fluid_contacts > 0) {
        for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
            for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
                int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
                AppendRow3(D_T, start_density + body_a, body_offset + body_b * 3, 0);
            }
            D_T.finalize(start_density + body_a);
        }
        // Add more entries for viscosity
        // Code is repeated because there are three rows per viscosity constraint
        if (enable_viscosity) {
            for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
                for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
                    int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
                    AppendRow3(D_T, start_viscous + body_a * 3 + 0, body_offset + body_b * 3, 0);
                }
                D_T.finalize(start_viscous + body_a * 3 + 0);
                //
                for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
                    int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
                    AppendRow3(D_T, start_viscous + body_a * 3 + 1, body_offset + body_b * 3, 0);
                }
                D_T.finalize(start_viscous + body_a * 3 + 1);
                //
                for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
                    int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
                    AppendRow3(D_T, start_viscous + body_a * 3 + 2, body_offset + body_b * 3, 0);
                }
                D_T.finalize(start_viscous + body_a * 3 + 2);
            }
        }
    }
}

void ChFluidContainer::PreSolve() {
    if (gamma_old.size() > 0) {
        if (enable_viscosity) {
            if (gamma_old.size() == (num_fluid_bodies + num_fluid_bodies * 3)) {
                blaze::subvector(data_manager->host_data.gamma, start_density,
                                 num_fluid_bodies + num_fluid_bodies * 3) = gamma_old * .9;
            }
        } else {
            if (gamma_old.size() == num_fluid_bodies) {
                blaze::subvector(data_manager->host_data.gamma, start_density, num_fluid_bodies) = gamma_old * .9;
            }
        }
    }
}

void ChFluidContainer::PostSolve() {
    if (num_fluid_bodies > 0) {
        if (enable_viscosity) {
            gamma_old.resize(num_fluid_bodies + num_fluid_bodies * 3);
            gamma_old =
                blaze::subvector(data_manager->host_data.gamma, start_density, num_fluid_bodies + num_fluid_bodies * 3);
        } else {
            gamma_old.resize(num_fluid_bodies);
            gamma_old = blaze::subvector(data_manager->host_data.gamma, start_density, num_fluid_bodies);
        }
    }

    if (artificial_pressure == false) {
        return;
    }
    custom_vector<real3>& sorted_pos = data_manager->host_data.sorted_pos_3dof;
    real h = kernel_radius;
    real k = artificial_pressure_k;
    real dq = artificial_pressure_dq;
    real n = artificial_pressure_n;
#pragma omp parallel for
    for (int body_a = 0; body_a < (signed)num_fluid_bodies; body_a++) {
        real corr = 0;
        real3 vorticity_grad(0);
        real3 pos_a = sorted_pos[body_a];
        for (int i = 0; i < data_manager->cd_data->c_counts_3dof_3dof[body_a]; i++) {
            int body_b = data_manager->cd_data->neighbor_3dof_3dof[body_a * ChNarrowphase::max_neighbors + i];
            if (body_a == body_b) {
                continue;
            }
            real3 xij = (pos_a - sorted_pos[body_b]);

            real dist = Length(xij);
            corr += k * Pow(KERNEL(dist, h) / KERNEL(dq, h), n);
        }

        data_manager->host_data.gamma[start_density + body_a] += corr;
    }
}

void ChFluidContainer::CalculateContactForces() {
    uint num_contacts = data_manager->cd_data->num_rigid_fluid_contacts;
    if (num_contacts <= 0) {
        return;
    }
    DynamicVector<real>& gamma = data_manager->host_data.gamma;
    SubVectorType gamma_n = subvector(gamma, start_boundary, _num_rf_c_);

    contact_forces = submatrix(data_manager->host_data.D, 0, start_boundary, _num_dof_, _num_rf_c_) * gamma_n /
                     data_manager->settings.step_size;

    if (contact_mu != 0) {
        SubVectorType gamma_t = subvector(gamma, start_boundary + _num_rf_c_, 2 * _num_rf_c_);
        contact_forces +=
            submatrix(data_manager->host_data.D, 0, start_boundary + _num_rf_c_, _num_dof_, 2 * _num_rf_c_) * gamma_t /
            data_manager->settings.step_size;
    }

    // contact_forces
}

real3 ChFluidContainer::GetBodyContactForce(std::shared_ptr<ChBody> body) {
    if (data_manager->cd_data->num_rigid_fluid_contacts <= 0) {
        return real3(0);
    }
    auto body_id = body->GetIndex();
    return real3(contact_forces[body_id * 6 + 0], contact_forces[body_id * 6 + 1], contact_forces[body_id * 6 + 2]);
}

real3 ChFluidContainer::GetBodyContactTorque(std::shared_ptr<ChBody> body) {
    if (data_manager->cd_data->num_rigid_fluid_contacts <= 0) {
        return real3(0);
    }
    auto body_id = body->GetIndex();
    return real3(contact_forces[body_id * 6 + 3], contact_forces[body_id * 6 + 4], contact_forces[body_id * 6 + 5]);
}

void ChFluidContainer::GetFluidDensity(custom_vector<real>& dens) {
    dens = density;
}
void ChFluidContainer::GetFluidPressure(custom_vector<real>& pres) {
    pres.resize(num_fluid_bodies);

    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        pres[i] = data_manager->host_data.gamma[start_density + i];
    }
}

void ChFluidContainer::GetFluidForce(custom_vector<real3>& forc) {
    forc.resize(num_fluid_bodies);

    DynamicVector<real>& gamma = data_manager->host_data.gamma;
    SubVectorType gamma_n = subvector(gamma, start_density, num_fluid_bodies);

    DynamicVector<real> pressure_forces =
        submatrix(data_manager->host_data.D, body_offset, start_density, num_fluid_bodies * 3, num_fluid_bodies) *
        gamma_n / data_manager->settings.step_size;

    for (int i = 0; i < (signed)num_fluid_bodies; i++) {
        forc[i] = real3(pressure_forces[i * 3 + 0], pressure_forces[i * 3 + 1], pressure_forces[i * 3 + 2]);
    }
}

}  // end namespace chrono

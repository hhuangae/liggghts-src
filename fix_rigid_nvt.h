/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(rigid/nvt,FixRigidNVT)

#else

#ifndef LMP_FIX_RIGID_NVT_H
#define LMP_FIX_RIGID_NVT_H

#include "fix_rigid.h"

namespace LAMMPS_NS {

class FixRigidNVT : public FixRigid {
 public:
  FixRigidNVT(class LAMMPS *, int, char **);
  ~FixRigidNVT();
  void init();
  void setup(int);
  void initial_integrate(int);
  void final_integrate();
  double compute_scalar();
  void write_restart(FILE *);
  void restart(char *);
  void reset_target(double);
  
 private:
  double **conjqm;                      // conjugate quaternion momentum
  double boltz;                         // boltzman constant
  double t_target;

  int nf_t,nf_r;                        // trans/rot degrees of freedom
  double *w,*wdti1,*wdti2,*wdti4;       // Yoshida-Suzuki coefficients
  double *q_t,*q_r;                     // trans/rot thermostat masses
  double *eta_t,*eta_r;                 // trans/rot thermostat positions
  double *eta_dot_t,*eta_dot_r;         // trans/rot thermostat velocities
  double *f_eta_t,*f_eta_r;             // trans/rot thermostat forces

  void update_nhcp(double, double);
  inline double maclaurin_series(double);

  void allocate_chain();
  void allocate_order();
  void deallocate_chain();
  void deallocate_order();
};

inline double FixRigidNVT::maclaurin_series(double x)
{
  double x2,x4;
  x2 = x * x;
  x4 = x2 * x2;
  return (1.0 + (1.0/6.0) * x2 + (1.0/120.0) * x4 + (1.0/5040.0) * x2 * x4 +
         (1.0/362880.0) * x4 * x4);
}

}

#endif
#endif

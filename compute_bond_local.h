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

#ifndef COMPUTE_BOND_LOCAL_H
#define COMPUTE_BOND_LOCAL_H

#include "compute.h"

namespace LAMMPS_NS {

class ComputeBondLocal : public Compute {
 public:
  ComputeBondLocal(class LAMMPS *, int, char **);
  ~ComputeBondLocal();
  void init();
  void compute_local();
  double memory_usage();

 private:
  int nvalues,dflag,eflag;
  int ncount;

  int nmax;
  double *vector;
  double **array;

  int compute_bonds(int);
  void reallocate(int);
};

}

#endif

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

FixStyle(external,FixExternal)

#else

#ifndef LMP_FIX_EXTERNAL_H
#define LMP_FIX_EXTERNAL_H

#include "fix.h"

namespace LAMMPS_NS {

class FixExternal : public Fix {
 public:
  FixExternal(class LAMMPS *, int, char **);
  ~FixExternal();
  int setmask();
  void init();
  void setup(int);
  void post_force(int);

  typedef void (*FnPtr)(void *, int, int, int *, double **, double **);
  void set_callback(FnPtr, void *);

 private:
  FnPtr callback;
  void *ptr_caller;
  int nmax;
  double **fexternal;
};

}

#endif
#endif
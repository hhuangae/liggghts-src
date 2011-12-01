/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Axel Kohlmeyer (Temple U)
------------------------------------------------------------------------- */

#ifdef IMPROPER_CLASS

ImproperStyle(harmonic/omp,ImproperHarmonicOMP)

#else

#ifndef LMP_IMPROPER_HARMONIC_OMP_H
#define LMP_IMPROPER_HARMONIC_OMP_H

#include "improper_harmonic.h"
#include "thr_omp.h"

namespace LAMMPS_NS {

class ImproperHarmonicOMP : public ImproperHarmonic, public ThrOMP {

 public:
    ImproperHarmonicOMP(class LAMMPS *lmp) : 
      ImproperHarmonic(lmp), ThrOMP(lmp,THR_IMPROPER) {};

  virtual void compute(int, int);

 private:
  template <int EVFLAG, int EFLAG, int NEWTON_BOND>
  void eval(int ifrom, int ito, ThrData * const thr);
};

}

#endif
#endif

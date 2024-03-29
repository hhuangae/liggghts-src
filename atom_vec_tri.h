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

#ifdef ATOM_CLASS

AtomStyle(tri,AtomVecTri)

#else

#ifndef LMP_ATOM_VEC_TRI_H
#define LMP_ATOM_VEC_TRI_H

#include "atom_vec.h"

namespace LAMMPS_NS {

class AtomVecTri : public AtomVec {
 public:
  struct Bonus {
    double quat[4];
    double c1[3],c2[3],c3[3];
    double inertia[3];
    int ilocal;
  };
  struct Bonus *bonus;

  AtomVecTri(class LAMMPS *, int, char **);
  ~AtomVecTri();
  void init();
  void grow(int);
  void grow_reset();
  void copy(int, int, int);
  int pack_comm(int, int *, double *, int, int *);
  int pack_comm_vel(int, int *, double *, int, int *);
  int pack_comm_hybrid(int, int *, double *);
  void unpack_comm(int, int, double *);
  void unpack_comm_vel(int, int, double *);
  int unpack_comm_hybrid(int, int, double *);
  int pack_reverse(int, int, double *);
  int pack_reverse_hybrid(int, int, double *);
  void unpack_reverse(int, int *, double *);
  int unpack_reverse_hybrid(int, int *, double *);
  int pack_border(int, int *, double *, int, int *);
  int pack_border_vel(int, int *, double *, int, int *);
  int pack_border_hybrid(int, int *, double *);
  void unpack_border(int, int, double *);
  void unpack_border_vel(int, int, double *);
  int unpack_border_hybrid(int, int, double *);
  int pack_exchange(int, double *);
  int unpack_exchange(double *);
  int size_restart();
  int pack_restart(int, double *);
  int unpack_restart(double *);
  void create_atom(int, double *);
  void data_atom(double *, int, char **);
  int data_atom_hybrid(int, char **);
  void data_vel(int, char **);
  int data_vel_hybrid(int, char **);
  bigint memory_usage();

  // manipulate Bonus data structure for extra atom info

  void clear_bonus();
  void data_atom_bonus(int, char **);

  // unique to AtomVecTri

  void set_equilateral(int, double);

 private:
  int *tag,*type,*mask,*image;
  double **x,**v,**f;
  int *molecule;
  double *rmass;
  double **angmom,**torque;
  int *tri;

  int nlocal_bonus,nghost_bonus,nmax_bonus;

  void grow_bonus();
  void copy_bonus(int, int);
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Atom_style tri can only be used in 3d simulations

Self-explanatory.

E: Per-processor system is too big

The number of owned atoms plus ghost atoms on a single
processor must fit in 32-bit integer.

E: Invalid atom ID in Atoms section of data file

Atom IDs must be positive integers.

E: Invalid atom type in Atoms section of data file

Atom types must range from 1 to specified # of types.

E: Invalid density in Atoms section of data file

Density value cannot be <= 0.0.

E: Assigning tri parameters to non-tri atom

Self-explanatory.

E: Invalid shape in Triangles section of data file

Two or more of the triangle corners are duplicate points.

E: Inconsistent triangle in data file

The centroid of the triangle as defined by the corner points is not
the atom coordinate.

E: Insufficient Jacobi rotations for triangle

The calculation of the intertia tensor of the triangle failed.  This
should not happen if it is a reasonably shaped triangle.

*/

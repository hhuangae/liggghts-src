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

#include "math.h"
#include "stdlib.h"
#include "string.h"
#include "region_sphere.h"
#include "error.h"

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

RegSphere::RegSphere(LAMMPS *lmp, int narg, char **arg) :
  Region(lmp, narg, arg)
{
  options(narg-6,&arg[6]);

  xc = xscale*atof(arg[2]);
  yc = yscale*atof(arg[3]);
  zc = zscale*atof(arg[4]);
  radius = xscale*atof(arg[5]);

  // error check

  if (radius < 0.0) error->all(FLERR,"Illegal region sphere command");

  // extent of sphere

  if (interior) {
    bboxflag = 1;
    extent_xlo = xc - radius;
    extent_xhi = xc + radius;
    extent_ylo = yc - radius;
    extent_yhi = yc + radius;
    extent_zlo = zc - radius;
    extent_zhi = zc + radius;
  } else bboxflag = 0;

  cmax = 1;
  contact = new Contact[cmax];
}

/* ---------------------------------------------------------------------- */

RegSphere::~RegSphere()
{
  delete [] contact;
}

/* ----------------------------------------------------------------------
   inside = 1 if x,y,z is inside or on surface
   inside = 0 if x,y,z is outside and not on surface
------------------------------------------------------------------------- */

int RegSphere::inside(double x, double y, double z)
{
  double delx = x - xc;
  double dely = y - yc;
  double delz = z - zc;
  double r = sqrt(delx*delx + dely*dely + delz*delz);

  if (r <= radius) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   one contact if 0 <= x < cutoff from inner surface of sphere
   no contact if outside (possible if called from union/intersect)
   delxyz = vector from nearest point on sphere to x
   special case: no contact if x is at center of sphere
------------------------------------------------------------------------- */

int RegSphere::surface_interior(double *x, double cutoff)
{
  double delx = x[0] - xc;
  double dely = x[1] - yc;
  double delz = x[2] - zc;
  double r = sqrt(delx*delx + dely*dely + delz*delz);
  if (r > radius || r == 0.0) return 0;

  double delta = radius - r;
  if (delta < cutoff) {
    contact[0].r = delta;
    contact[0].delx = delx*(1.0-radius/r);
    contact[0].dely = dely*(1.0-radius/r);
    contact[0].delz = delz*(1.0-radius/r);
    return 1;
  }
  return 0;
}

/* ----------------------------------------------------------------------
   one contact if 0 <= x < cutoff from outer surface of sphere
   no contact if inside (possible if called from union/intersect)
   delxyz = vector from nearest point on sphere to x
------------------------------------------------------------------------- */

int RegSphere::surface_exterior(double *x, double cutoff)
{
  double delx = x[0] - xc;
  double dely = x[1] - yc;
  double delz = x[2] - zc;
  double r = sqrt(delx*delx + dely*dely + delz*delz);
  if (r < radius) return 0;

  double delta = r - radius;
  if (delta < cutoff) {
    contact[0].r = delta;
    contact[0].delx = delx*(1.0-radius/r);
    contact[0].dely = dely*(1.0-radius/r);
    contact[0].delz = delz*(1.0-radius/r);
    return 1;
  }
  return 0;
}

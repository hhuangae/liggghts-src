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
#include "fix_dt_reset.h"
#include "atom.h"
#include "update.h"
#include "integrate.h"
#include "domain.h"
#include "lattice.h"
#include "force.h"
#include "pair.h"
#include "modify.h"
#include "fix.h"
#include "output.h"
#include "dump.h"
#include "comm.h"
#include "error.h"

using namespace LAMMPS_NS;
using namespace FixConst;

#define BIG 1.0e20

/* ---------------------------------------------------------------------- */

FixDtReset::FixDtReset(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if (narg < 7) error->all(FLERR,"Illegal fix dt/reset command");

  time_depend = 1;
  scalar_flag = 1;
  vector_flag = 1;
  size_vector = 2;
  global_freq = 1;
  extscalar = 0;
  extvector = 0;

  nevery = atoi(arg[3]);
  if (nevery <= 0) error->all(FLERR,"Illegal fix dt/reset command");

  minbound = maxbound = 1;
  tmin = tmax = 0.0;
  if (strcmp(arg[4],"NULL") == 0) minbound = 0;
  else tmin = atof(arg[4]);
  if (strcmp(arg[5],"NULL") == 0) maxbound = 0;
  else tmax = atof(arg[5]);
  xmax = atof(arg[6]);

  if (minbound && tmin < 0.0) error->all(FLERR,"Illegal fix dt/reset command");
  if (maxbound && tmax < 0.0) error->all(FLERR,"Illegal fix dt/reset command");
  if (minbound && maxbound && tmin >= tmax)
    error->all(FLERR,"Illegal fix dt/reset command");
  if (xmax <= 0.0) error->all(FLERR,"Illegal fix dt/reset command");

  int scaleflag = 1;

  int iarg = 7;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"units") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal fix dt/reset command");
      if (strcmp(arg[iarg+1],"box") == 0) scaleflag = 0;
      else if (strcmp(arg[iarg+1],"lattice") == 0) scaleflag = 1;
      else error->all(FLERR,"Illegal fix dt/reset command");
      iarg += 2;
    } else error->all(FLERR,"Illegal fix dt/reset command");
  }

  // setup scaling, based on xlattice parameter

  if (scaleflag && domain->lattice == NULL)
    error->all(FLERR,"Use of fix dt/reset with undefined lattice");
  if (scaleflag) xmax *= domain->lattice->xlattice;

  // initializations

  t_elapsed = t_laststep = 0.0;
  laststep = update->ntimestep;
}

/* ---------------------------------------------------------------------- */

int FixDtReset::setmask()
{
  int mask = 0;
  mask |= INITIAL_INTEGRATE;
  mask |= END_OF_STEP;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixDtReset::init()
{
  // set rRESPA flag

  respaflag = 0;
  if (strstr(update->integrate_style,"respa")) respaflag = 1;

  // check for DCD or XTC dumps

  for (int i = 0; i < output->ndump; i++)
    if ((strcmp(output->dump[i]->style,"dcd") == 0 ||
	strcmp(output->dump[i]->style,"xtc") == 0) && comm->me == 0)
      error->warning(FLERR,
		     "Dump dcd/xtc timestamp may be wrong with fix dt/reset");

  ftm2v = force->ftm2v;
  dt = update->dt;
}

/* ---------------------------------------------------------------------- */

void FixDtReset::setup(int vflag)
{
  end_of_step();
}

/* ---------------------------------------------------------------------- */

void FixDtReset::initial_integrate(int vflag)
{
  // calculate elapsed time based on previous reset timestep

  t_elapsed = t_laststep + (update->ntimestep-laststep)*dt;
}

/* ---------------------------------------------------------------------- */

void FixDtReset::end_of_step()
{
  double dtv,dtf,dtsq;
  double vsq,fsq,massinv;
  double delx,dely,delz,delr;

  // compute vmax and amax of any atom in group

  double **v = atom->v;
  double **f = atom->f;
  double *mass = atom->mass;
  double *rmass = atom->rmass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  double dtmin = BIG;

  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      if (rmass) massinv = 1.0/rmass[i];
      else massinv = 1.0/mass[type[i]];
      vsq = v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2];
      fsq = f[i][0]*f[i][0] + f[i][1]*f[i][1] + f[i][2]*f[i][2];
      dtv = dtf = BIG;
      if (vsq > 0.0) dtv = xmax/sqrt(vsq);
      if (fsq > 0.0) dtf = sqrt(2.0*xmax/(ftm2v*sqrt(fsq)*massinv));
      dt = MIN(dtv,dtf);
      dtsq = dt*dt;
      delx = dt*v[i][0] + 0.5*dtsq*massinv*f[i][0] * ftm2v;
      dely = dt*v[i][1] + 0.5*dtsq*massinv*f[i][1] * ftm2v;
      delz = dt*v[i][2] + 0.5*dtsq*massinv*f[i][2] * ftm2v;
      delr = sqrt(delx*delx + dely*dely + delz*delz);
      if (delr > xmax) dt *= xmax/delr;
      dtmin = MIN(dtmin,dt);
    }

  MPI_Allreduce(&dtmin,&dt,1,MPI_DOUBLE,MPI_MIN,world);

  if (minbound) dt = MAX(dt,tmin);
  if (maxbound) dt = MIN(dt,tmax);
  
  // if timestep didn't change, just return
  // else reset update->dt and other classes that depend on it
  // rRESPA, pair style, fixes

  if (dt == update->dt) return;

  t_elapsed = t_laststep += (update->ntimestep-laststep)*update->dt;
  laststep = update->ntimestep;

  update->dt = dt;
  if (respaflag) update->integrate->reset_dt();
  if (force->pair) force->pair->reset_dt();
  for (int i = 0; i < modify->nfix; i++) modify->fix[i]->reset_dt();
}

/* ---------------------------------------------------------------------- */

double FixDtReset::compute_scalar()
{
  return update->dt;
}

/* ---------------------------------------------------------------------- */

double FixDtReset::compute_vector(int n)
{
  if (n == 0) return t_elapsed;
  return (double) laststep;
}

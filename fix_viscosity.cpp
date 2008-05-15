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
#include "mpi.h"
#include "string.h"
#include "stdlib.h"
#include "fix_viscosity.h"
#include "atom.h"
#include "domain.h"
#include "error.h"

using namespace LAMMPS_NS;

// needs to be big, but not so big that lose precision when subtract velocity

#define BIG 1.0e10

#define MIN(A,B) ((A) < (B)) ? (A) : (B)
#define MAX(A,B) ((A) > (B)) ? (A) : (B)

/* ---------------------------------------------------------------------- */

FixViscosity::FixViscosity(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
  if (narg < 7) error->all("Illegal fix viscosity command");

  MPI_Comm_rank(world,&me);

  nevery = atoi(arg[3]);
  if (nevery <= 0) error->all("Illegal fix viscosity command");

  scalar_flag = 1;
  scalar_vector_freq = nevery;
  extscalar = 0;

  if (strcmp(arg[4],"x") == 0) vdim = 0;
  else if (strcmp(arg[4],"y") == 0) vdim = 1;
  else if (strcmp(arg[4],"z") == 0) vdim = 2;
  else error->all("Illegal fix viscosity command");

  if (strcmp(arg[5],"x") == 0) pdim = 0;
  else if (strcmp(arg[5],"y") == 0) pdim = 1;
  else if (strcmp(arg[5],"z") == 0) pdim = 2;
  else error->all("Illegal fix viscosity command");

  nbin = atoi(arg[6]);
  if (nbin < 3) error->all("Illegal fix viscosity command");

  // optional keywords

  nswap = 1;
  vtarget = BIG;

  int iarg = 7;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"swap") == 0) {
      if (iarg+2 > narg) error->all("Illegal fix viscosity command");
      nswap = atoi(arg[iarg+1]);
      if (nswap <= 0) error->all("Fix viscosity swap value must be positive");
      iarg += 2;
    } else if (strcmp(arg[iarg],"vtarget") == 0) {
      if (iarg+2 > narg) error->all("Illegal fix viscosity command");
      if (strcmp(arg[iarg+1],"INF") == 0) vtarget = BIG;
      else vtarget = atof(arg[iarg+1]);
      if (vtarget <= 0.0)
	error->all("Fix viscosity vtarget value must be positive");
      iarg += 2;
    } else error->all("Illegal fix viscosity command");
  }

  // initialize array sizes to nswap+1 so have space to shift values down

  pos_index = new int[nswap+1];
  neg_index = new int[nswap+1];
  pos_delta = new double[nswap+1];
  neg_delta = new double[nswap+1];

  p_exchange = 0.0;
}

/* ---------------------------------------------------------------------- */

FixViscosity::~FixViscosity()
{
  delete [] pos_index;
  delete [] neg_index;
  delete [] pos_delta;
  delete [] neg_delta;
}

/* ---------------------------------------------------------------------- */

int FixViscosity::setmask()
{
  int mask = 0;
  mask |= END_OF_STEP;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixViscosity::init()
{
  // set bounds of 2 slabs in pdim
  // only necessary for static box, else re-computed in end_of_step()
  // lo bin is always bottom bin
  // if nbin even, hi bin is just below half height
  // if nbin odd, hi bin straddles half height

  if (domain->box_change == 0) {
    prd = domain->prd[pdim];
    boxlo = domain->boxlo[pdim];
    boxhi = domain->boxhi[pdim];
    double binsize = (boxhi-boxlo) / nbin;
    slablo_lo = boxlo;
    slablo_hi = boxlo + binsize;
    slabhi_lo = boxlo + ((nbin-1)/2)*binsize;
    slabhi_hi = boxlo + ((nbin-1)/2 + 1)*binsize;
  }

  periodicity = domain->periodicity[pdim];
}

/* ---------------------------------------------------------------------- */

void FixViscosity::end_of_step()
{
  int i,m,insert;
  double coord,delta;
  MPI_Status status;
  struct {
    double value;
    int proc;
  } mine[2],all[2];

  // if box changes, recompute bounds of 2 slabs in pdim

  if (domain->box_change) {
    prd = domain->prd[pdim];
    boxlo = domain->boxlo[pdim];
    boxhi = domain->boxhi[pdim];
    double binsize = (boxhi-boxlo) / nbin;
    slablo_lo = boxlo;
    slablo_hi = boxlo + binsize;
    slabhi_lo = boxlo + ((nbin-1)/2)*binsize;
    slabhi_hi = boxlo + ((nbin-1)/2 + 1)*binsize;
  }

  // make 2 lists of up to nswap atoms with velocity closest to +/- vtarget
  // lists are sorted by closeness to vtarget
  // only consider atoms in the bottom/middle slabs
  // map atoms back into periodic box if necessary
  // insert = location in list to insert new atom

  double **x = atom->x;
  double **v = atom->v;
  double *mass = atom->mass;
  double *rmass = atom->rmass;
  int *type = atom->type;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  npositive = nnegative = 0;

  for (i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      coord = x[i][pdim];
      if (coord < boxlo && periodicity) coord += prd;
      else if (coord >= boxhi && periodicity) coord -= prd;

      if (coord >= slablo_lo && coord < slablo_hi) {
	if (v[i][vdim] < 0.0) continue;
	delta = fabs(v[i][vdim] - vtarget);
	if (npositive < nswap || delta < pos_delta[nswap-1]) {
	  for (insert = npositive-1; insert >= 0; insert--)
	    if (delta > pos_delta[insert]) break;
	  insert++;
	  for (m = npositive-1; m >= insert; m--) {
	    pos_delta[m+1] = pos_delta[m];
	    pos_index[m+1] = pos_index[m];
	  }
	  pos_delta[insert] = delta;
	  pos_index[insert] = i;
	  if (npositive < nswap) npositive++;
	}
      }

      if (coord >= slabhi_lo && coord < slabhi_hi) {
	if (v[i][vdim] > 0.0) continue;
	delta = fabs(v[i][vdim] + vtarget);
	if (nnegative < nswap || delta < neg_delta[nswap-1]) {
	  for (insert = nnegative-1; insert >= 0; insert--)
	    if (delta > neg_delta[insert]) break;
	  insert++;
	  for (m = nnegative-1; m >= insert; m--) {
	    neg_delta[m+1] = neg_delta[m];
	    neg_index[m+1] = neg_index[m];
	  }
	  neg_delta[insert] = delta;
	  neg_index[insert] = i;
	  if (nnegative < nswap) nnegative++;
	}
      }
    }

  // loop over nswap pairs
  // find 2 global atoms with smallest delta in bottom/top slabs
  // BIG values are for procs with no atom to contribute
  // MINLOC also communicates which procs own them
  // exchange momenta between the 2 particles
  // if I own both particles just swap, else point2point comm of vel,mass

  int ipos,ineg;
  double sbuf[2],rbuf[2];

  double pswap = 0.0;
  mine[0].proc = mine[1].proc = me;
  int ipositive = 0;
  int inegative = 0;

  for (m = 0; m < nswap; m++) {
    if (ipositive < npositive) mine[0].value = pos_delta[ipositive];
    else mine[0].value = BIG;
    if (inegative < nnegative) mine[1].value = neg_delta[inegative];
    else mine[1].value = BIG;
    
    MPI_Allreduce(mine,all,2,MPI_DOUBLE_INT,MPI_MINLOC,world);

    if (all[0].value == BIG || all[1].value == BIG) continue;

    if (me == all[0].proc && me == all[1].proc) {
      ipos = pos_index[ipositive++];
      ineg = neg_index[inegative++];
      rbuf[0] = v[ipos][vdim];
      if (mass) rbuf[1] = mass[type[ipos]];
      else rbuf[1] = rmass[ipos];
      sbuf[0] = v[ineg][vdim];
      if (mass) sbuf[1] = mass[type[ineg]];
      else sbuf[1] = rmass[ineg];
      v[ineg][vdim] = rbuf[0] * rbuf[1]/sbuf[1];
      v[ipos][vdim] = sbuf[0] * sbuf[1]/rbuf[1];
      pswap += rbuf[0]*rbuf[1] - sbuf[0]*sbuf[1];
      
    } else if (me == all[0].proc) {
      ipos = pos_index[ipositive++];
      sbuf[0] = v[ipos][vdim];
      if (mass) sbuf[1] = mass[type[ipos]];
      else sbuf[1] = rmass[ipos];
      MPI_Sendrecv(sbuf,2,MPI_DOUBLE,all[1].proc,0,
		   rbuf,2,MPI_DOUBLE,all[1].proc,0,world,&status);
      v[ipos][vdim] = rbuf[0] * rbuf[1]/sbuf[1];
      pswap += sbuf[0]*sbuf[1];

    } else if (me == all[1].proc) {
      ineg = neg_index[inegative++];
      sbuf[0] = v[ineg][vdim];
      if (mass) sbuf[1] = mass[type[ineg]];
      else sbuf[1] = rmass[ineg];
      MPI_Sendrecv(sbuf,2,MPI_DOUBLE,all[0].proc,0,
		   rbuf,2,MPI_DOUBLE,all[0].proc,0,world,&status);
      v[ineg][vdim] = rbuf[0] * rbuf[1]/sbuf[1];
      pswap -= sbuf[0]*sbuf[1];
    }
  }

  // tally momentum exchange from all swaps

  double pswap_all;
  MPI_Allreduce(&pswap,&pswap_all,1,MPI_DOUBLE,MPI_SUM,world);
  p_exchange += pswap_all;
}

/* ---------------------------------------------------------------------- */

double FixViscosity::compute_scalar()
{
  return p_exchange;
}

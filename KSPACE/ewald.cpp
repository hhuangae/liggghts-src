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

/* ----------------------------------------------------------------------
   Contributing authors: Roy Pollock (LLNL), Paul Crozier (SNL)
     per-atom energy/virial added by German Samolyuk (ORNL), Stan Moore (BYU)
------------------------------------------------------------------------- */

#include "mpi.h"
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "math.h"
#include "ewald.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "pair.h"
#include "domain.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;
using namespace MathConst;

#define SMALL 0.00001

/* ---------------------------------------------------------------------- */

Ewald::Ewald(LAMMPS *lmp, int narg, char **arg) : KSpace(lmp, narg, arg)
{
  if (narg != 1) error->all(FLERR,"Illegal kspace_style ewald command");

  accuracy_relative = atof(arg[0]);

  kmax = 0;
  kxvecs = kyvecs = kzvecs = NULL;
  ug = NULL;
  eg = vg = NULL;
  sfacrl = sfacim = sfacrl_all = sfacim_all = NULL;

  nmax = 0;
  ek = NULL;
  cs = sn = NULL;

  kcount = 0;
}

/* ----------------------------------------------------------------------
   free all memory 
------------------------------------------------------------------------- */

Ewald::~Ewald()
{
  deallocate();
  memory->destroy(ek);
  memory->destroy3d_offset(cs,-kmax_created);
  memory->destroy3d_offset(sn,-kmax_created);
}

/* ---------------------------------------------------------------------- */

void Ewald::init()
{
  if (comm->me == 0) {
    if (screen) fprintf(screen,"Ewald initialization ...\n");
    if (logfile) fprintf(logfile,"Ewald initialization ...\n");
  }

  // error check

  if (domain->triclinic) 
    error->all(FLERR,"Cannot use Ewald with triclinic box");
  if (domain->dimension == 2) 
    error->all(FLERR,"Cannot use Ewald with 2d simulation");

  if (!atom->q_flag) error->all(FLERR,"Kspace style requires atom attribute q");

  if (slabflag == 0 && domain->nonperiodic > 0)
    error->all(FLERR,"Cannot use nonperiodic boundaries with Ewald");
  if (slabflag == 1) {
    if (domain->xperiodic != 1 || domain->yperiodic != 1 || 
	domain->boundary[2][0] != 1 || domain->boundary[2][1] != 1)
      error->all(FLERR,"Incorrect boundaries with slab Ewald");
  }

  // extract short-range Coulombic cutoff from pair style

  scale = 1.0;

  if (force->pair == NULL)
    error->all(FLERR,"KSpace style is incompatible with Pair style");
  int itmp;
  double *p_cutoff = (double *) force->pair->extract("cut_coul",itmp);
  if (p_cutoff == NULL)
    error->all(FLERR,"KSpace style is incompatible with Pair style");
  double cutoff = *p_cutoff;

  qsum = qsqsum = 0.0;
  for (int i = 0; i < atom->nlocal; i++) {
    qsum += atom->q[i];
    qsqsum += atom->q[i]*atom->q[i];
  }

  double tmp;
  MPI_Allreduce(&qsum,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
  qsum = tmp;
  MPI_Allreduce(&qsqsum,&tmp,1,MPI_DOUBLE,MPI_SUM,world);
  qsqsum = tmp;

  if (qsqsum == 0.0)
    error->all(FLERR,"Cannot use kspace solver on system with no charge");
  if (fabs(qsum) > SMALL && comm->me == 0) {
    char str[128];
    sprintf(str,"System is not charge neutral, net charge = %g",qsum);
    error->warning(FLERR,str);
  }

  // set accuracy (force units) from accuracy_relative or accuracy_absolute
  
  if (accuracy_absolute >= 0.0) accuracy = accuracy_absolute;
  else accuracy = accuracy_relative * two_charge_force;
  
  // setup K-space resolution

  q2 = qsqsum * force->qqrd2e / force->dielectric;
  bigint natoms = atom->natoms;

  // use xprd,yprd,zprd even if triclinic so grid size is the same
  // adjust z dimension for 2d slab Ewald
  // 3d Ewald just uses zprd since slab_volfactor = 1.0

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;
  double zprd_slab = zprd*slab_volfactor;
  
  // make initial g_ewald estimate
  // based on desired accuracy and real space cutoff
  // fluid-occupied volume used to estimate real-space error
  // zprd used rather than zprd_slab

  if (!gewaldflag)
    g_ewald = sqrt(-log(accuracy*sqrt(natoms*cutoff*xprd*yprd*zprd) / 
                        (2.0*q2))) / cutoff;

  // setup Ewald coefficients so can print stats

  setup();

  // final RMS accuracy

  double lprx = rms(kxmax,xprd,natoms,q2);
  double lpry = rms(kymax,yprd,natoms,q2);
  double lprz = rms(kzmax,zprd_slab,natoms,q2);
  double lpr = sqrt(lprx*lprx + lpry*lpry + lprz*lprz) / sqrt(3.0);
  double spr = 2.0*q2 * exp(-g_ewald*g_ewald*cutoff*cutoff) / 
    sqrt(natoms*cutoff*xprd*yprd*zprd_slab);
  
  // stats

  if (comm->me == 0) {
    if (screen) {
      fprintf(screen,"  G vector (1/distance) = %g\n",g_ewald);
      fprintf(screen,"  estimated absolute RMS force accuracy = %g\n",
	      MAX(lpr,spr));
      fprintf(screen,"  estimated relative force accuracy = %g\n",
	      MAX(lpr,spr)/two_charge_force);
      fprintf(screen,"  KSpace vectors: actual max1d max3d = %d %d %d\n",
	      kcount,kmax,kmax3d);
    }
    if (logfile) {
      fprintf(logfile,"  G vector (1/distnace) = %g\n",g_ewald);
      fprintf(logfile,"  estimated absolute RMS force accuracy = %g\n",
	      MAX(lpr,spr));
      fprintf(logfile,"  estimated relative force accuracy = %g\n",
	      MAX(lpr,spr)/two_charge_force);
      fprintf(logfile,"  KSpace vectors: actual max1d max3d = %d %d %d\n",
	      kcount,kmax,kmax3d);
    }
  }
}

/* ----------------------------------------------------------------------
   adjust Ewald coeffs, called initially and whenever volume has changed 
------------------------------------------------------------------------- */

void Ewald::setup()
{
  // volume-dependent factors

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;
  
  // adjustment of z dimension for 2d slab Ewald
  // 3d Ewald just uses zprd since slab_volfactor = 1.0

  double zprd_slab = zprd*slab_volfactor;
  volume = xprd * yprd * zprd_slab;

  unitk[0] = 2.0*MY_PI/xprd;
  unitk[1] = 2.0*MY_PI/yprd;
  unitk[2] = 2.0*MY_PI/zprd_slab;

  // determine kmax
  // function of current box size, accuracy, G_ewald (short-range cutoff)

  bigint natoms = atom->natoms;
  double err;
  kxmax = 1;
  kymax = 1;
  kzmax = 1;
  
  err = rms(kxmax,xprd,natoms,q2);
  while (err > accuracy) {
    kxmax++;
    err = rms(kxmax,xprd,natoms,q2);
  }

  err = rms(kymax,yprd,natoms,q2);
  while (err > accuracy) {
    kymax++;
    err = rms(kymax,yprd,natoms,q2);
  }

  err = rms(kzmax,zprd_slab,natoms,q2);
  while (err > accuracy) {
    kzmax++;
    err = rms(kzmax,zprd_slab,natoms,q2);
  } 

  int kmax_old = kmax;
  kmax = MAX(kxmax,kymax);
  kmax = MAX(kmax,kzmax);
  kmax3d = 4*kmax*kmax*kmax + 6*kmax*kmax + 3*kmax;

  double gsqxmx = unitk[0]*unitk[0]*kxmax*kxmax;
  double gsqymx = unitk[1]*unitk[1]*kymax*kymax;
  double gsqzmx = unitk[2]*unitk[2]*kzmax*kzmax;
  gsqmx = MAX(gsqxmx,gsqymx);
  gsqmx = MAX(gsqmx,gsqzmx);
  
  // if size has grown, reallocate k-dependent and nlocal-dependent arrays

  if (kmax > kmax_old) {
    deallocate();
    allocate();

    memory->destroy(ek);
    memory->destroy3d_offset(cs,-kmax_created);
    memory->destroy3d_offset(sn,-kmax_created);
    nmax = atom->nmax;
    memory->create(ek,nmax,3,"ewald:ek");
    memory->create3d_offset(cs,-kmax,kmax,3,nmax,"ewald:cs");
    memory->create3d_offset(sn,-kmax,kmax,3,nmax,"ewald:sn");
    kmax_created = kmax;
  }

  // pre-compute Ewald coefficients

  coeffs();
}

/* ----------------------------------------------------------------------
   compute RMS accuracy for a dimension
------------------------------------------------------------------------- */

double Ewald::rms(int km, double prd, bigint natoms, double q2)
{
  double value = 2.0*q2*g_ewald/prd * 
    sqrt(1.0/(MY_PI*km*natoms)) * 
    exp(-MY_PI*MY_PI*km*km/(g_ewald*g_ewald*prd*prd));

  return value;
}

/* ----------------------------------------------------------------------
   compute the Ewald long-range force, energy, virial 
------------------------------------------------------------------------- */

void Ewald::compute(int eflag, int vflag)
{
  int i,j,k;

  // set energy/virial flags

  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = evflag_atom = eflag_global = vflag_global = 
	 eflag_atom = vflag_atom = 0;

  // extend size of per-atom arrays if necessary

  if (atom->nlocal > nmax) {
    memory->destroy(ek);
    memory->destroy3d_offset(cs,-kmax_created);
    memory->destroy3d_offset(sn,-kmax_created);
    nmax = atom->nmax;
    memory->create(ek,nmax,3,"ewald:ek");
    memory->create3d_offset(cs,-kmax,kmax,3,nmax,"ewald:cs");
    memory->create3d_offset(sn,-kmax,kmax,3,nmax,"ewald:sn");
    kmax_created = kmax;
  }

  // partial structure factors on each processor
  // total structure factor by summing over procs

  eik_dot_r();
  MPI_Allreduce(sfacrl,sfacrl_all,kcount,MPI_DOUBLE,MPI_SUM,world);
  MPI_Allreduce(sfacim,sfacim_all,kcount,MPI_DOUBLE,MPI_SUM,world);

  // K-space portion of electric field
  // double loop over K-vectors and local atoms
  // perform per-atom calculations if needed

  double **f = atom->f;
  double *q = atom->q;
  int nlocal = atom->nlocal;

  int kx,ky,kz;
  double cypz,sypz,exprl,expim,partial,partial_peratom;

  for (i = 0; i < nlocal; i++) {
    ek[i][0] = 0.0;
    ek[i][1] = 0.0;
    ek[i][2] = 0.0;
  }

  for (k = 0; k < kcount; k++) {
    kx = kxvecs[k];
    ky = kyvecs[k];
    kz = kzvecs[k];

    for (i = 0; i < nlocal; i++) {
      cypz = cs[ky][1][i]*cs[kz][2][i] - sn[ky][1][i]*sn[kz][2][i];
      sypz = sn[ky][1][i]*cs[kz][2][i] + cs[ky][1][i]*sn[kz][2][i];
      exprl = cs[kx][0][i]*cypz - sn[kx][0][i]*sypz;
      expim = sn[kx][0][i]*cypz + cs[kx][0][i]*sypz;
      partial = expim*sfacrl_all[k] - exprl*sfacim_all[k];
      ek[i][0] += partial*eg[k][0];
      ek[i][1] += partial*eg[k][1];
      ek[i][2] += partial*eg[k][2];

      if (evflag_atom) {
        partial_peratom = exprl*sfacrl_all[k] + expim*sfacim_all[k];		
    	if (eflag_atom) eatom[i] += q[i]*ug[k]*partial_peratom;
        if (vflag_atom)
	  for (j = 0; j < 6; j++)
	    vatom[i][j] += ug[k]*vg[k][j]*partial_peratom;
      }
    }
  }

  // convert E-field to force

  const double qscale = force->qqrd2e * scale;

  for (i = 0; i < nlocal; i++) {
    f[i][0] += qscale * q[i]*ek[i][0];
    f[i][1] += qscale * q[i]*ek[i][1];
    f[i][2] += qscale * q[i]*ek[i][2];
  }
 
  // global energy

  if (eflag_global) {
    for (k = 0; k < kcount; k++)
      energy += ug[k] * (sfacrl_all[k]*sfacrl_all[k] + 
			 sfacim_all[k]*sfacim_all[k]);
    energy -= g_ewald*qsqsum/MY_PIS +
      MY_PI2*qsum*qsum / (g_ewald*g_ewald*volume);
    energy *= qscale;
  }

  // global virial

  if (vflag_global) {
    double uk;
    for (k = 0; k < kcount; k++) {
      uk = ug[k] * (sfacrl_all[k]*sfacrl_all[k] + sfacim_all[k]*sfacim_all[k]);
      for (j = 0; j < 6; j++) virial[j] += uk*vg[k][j];
    }
    for (j = 0; j < 6; j++) virial[j] *= qscale;
  }

  // per-atom energy/virial
  // energy includes self-energy correction

  if (evflag_atom) {
    if (eflag_atom) {
      for (i = 0; i < nlocal; i++) {
        eatom[i] -= g_ewald*q[i]*q[i]/MY_PIS + MY_PI2*q[i]*qsum / 
	  (g_ewald*g_ewald*volume);
        eatom[i] *= qscale;
      }
    }
      
    if (vflag_atom)
      for (i = 0; i < nlocal; i++)
	for (j = 0; j < 6; j++) vatom[i][j] *= q[i]*qscale;
  }

  // 2d slab correction

  if (slabflag) slabcorr();
}

/* ---------------------------------------------------------------------- */

void Ewald::eik_dot_r()
{
  int i,k,l,m,n,ic;
  double cstr1,sstr1,cstr2,sstr2,cstr3,sstr3,cstr4,sstr4;
  double sqk,clpm,slpm;

  double **x = atom->x;
  double *q = atom->q;
  int nlocal = atom->nlocal;

  n = 0;

  // (k,0,0), (0,l,0), (0,0,m)

  for (ic = 0; ic < 3; ic++) {
    sqk = unitk[ic]*unitk[ic];
    if (sqk <= gsqmx) {
      cstr1 = 0.0;
      sstr1 = 0.0;
      for (i = 0; i < nlocal; i++) {
	cs[0][ic][i] = 1.0;
	sn[0][ic][i] = 0.0;
	cs[1][ic][i] = cos(unitk[ic]*x[i][ic]);
	sn[1][ic][i] = sin(unitk[ic]*x[i][ic]);
	cs[-1][ic][i] = cs[1][ic][i];
	sn[-1][ic][i] = -sn[1][ic][i];
	cstr1 += q[i]*cs[1][ic][i];
	sstr1 += q[i]*sn[1][ic][i];
      }
      sfacrl[n] = cstr1;
      sfacim[n++] = sstr1;
    }
  }

  for (m = 2; m <= kmax; m++) {
    for (ic = 0; ic < 3; ic++) {
      sqk = m*unitk[ic] * m*unitk[ic];
      if (sqk <= gsqmx) {
	cstr1 = 0.0;
	sstr1 = 0.0;
	for (i = 0; i < nlocal; i++) {
	  cs[m][ic][i] = cs[m-1][ic][i]*cs[1][ic][i] - 
	    sn[m-1][ic][i]*sn[1][ic][i];
	  sn[m][ic][i] = sn[m-1][ic][i]*cs[1][ic][i] + 
	    cs[m-1][ic][i]*sn[1][ic][i];
	  cs[-m][ic][i] = cs[m][ic][i];
	  sn[-m][ic][i] = -sn[m][ic][i];
	  cstr1 += q[i]*cs[m][ic][i];
	  sstr1 += q[i]*sn[m][ic][i];
	}
	sfacrl[n] = cstr1;
	sfacim[n++] = sstr1;
      }
    }
  }

  // 1 = (k,l,0), 2 = (k,-l,0)

  for (k = 1; k <= kxmax; k++) {
    for (l = 1; l <= kymax; l++) {
      sqk = (k*unitk[0] * k*unitk[0]) + (l*unitk[1] * l*unitk[1]);
      if (sqk <= gsqmx) {
	cstr1 = 0.0;
	sstr1 = 0.0;
	cstr2 = 0.0;
	sstr2 = 0.0;
	for (i = 0; i < nlocal; i++) {
	  cstr1 += q[i]*(cs[k][0][i]*cs[l][1][i] - sn[k][0][i]*sn[l][1][i]);
	  sstr1 += q[i]*(sn[k][0][i]*cs[l][1][i] + cs[k][0][i]*sn[l][1][i]);
	  cstr2 += q[i]*(cs[k][0][i]*cs[l][1][i] + sn[k][0][i]*sn[l][1][i]);
	  sstr2 += q[i]*(sn[k][0][i]*cs[l][1][i] - cs[k][0][i]*sn[l][1][i]);
	}
	sfacrl[n] = cstr1;
	sfacim[n++] = sstr1;
	sfacrl[n] = cstr2;
	sfacim[n++] = sstr2;
      }
    }
  }

  // 1 = (0,l,m), 2 = (0,l,-m)

  for (l = 1; l <= kymax; l++) {
    for (m = 1; m <= kzmax; m++) {
      sqk = (l*unitk[1] * l*unitk[1]) + (m*unitk[2] * m*unitk[2]);
      if (sqk <= gsqmx) {
	cstr1 = 0.0;
	sstr1 = 0.0;
	cstr2 = 0.0;
	sstr2 = 0.0;
	for (i = 0; i < nlocal; i++) {
	  cstr1 += q[i]*(cs[l][1][i]*cs[m][2][i] - sn[l][1][i]*sn[m][2][i]);
	  sstr1 += q[i]*(sn[l][1][i]*cs[m][2][i] + cs[l][1][i]*sn[m][2][i]);
	  cstr2 += q[i]*(cs[l][1][i]*cs[m][2][i] + sn[l][1][i]*sn[m][2][i]);
	  sstr2 += q[i]*(sn[l][1][i]*cs[m][2][i] - cs[l][1][i]*sn[m][2][i]);
	}
	sfacrl[n] = cstr1;
	sfacim[n++] = sstr1;
	sfacrl[n] = cstr2;
	sfacim[n++] = sstr2;
      }
    }
  }

  // 1 = (k,0,m), 2 = (k,0,-m)

  for (k = 1; k <= kxmax; k++) {
    for (m = 1; m <= kzmax; m++) {
      sqk = (k*unitk[0] * k*unitk[0]) + (m*unitk[2] * m*unitk[2]);
      if (sqk <= gsqmx) {
	cstr1 = 0.0;
	sstr1 = 0.0;
	cstr2 = 0.0;
	sstr2 = 0.0;
	for (i = 0; i < nlocal; i++) {
	  cstr1 += q[i]*(cs[k][0][i]*cs[m][2][i] - sn[k][0][i]*sn[m][2][i]);
	  sstr1 += q[i]*(sn[k][0][i]*cs[m][2][i] + cs[k][0][i]*sn[m][2][i]);
	  cstr2 += q[i]*(cs[k][0][i]*cs[m][2][i] + sn[k][0][i]*sn[m][2][i]);
	  sstr2 += q[i]*(sn[k][0][i]*cs[m][2][i] - cs[k][0][i]*sn[m][2][i]);
	}
	sfacrl[n] = cstr1;
	sfacim[n++] = sstr1;
	sfacrl[n] = cstr2;
	sfacim[n++] = sstr2;
      }
    }
  }

  // 1 = (k,l,m), 2 = (k,-l,m), 3 = (k,l,-m), 4 = (k,-l,-m)

  for (k = 1; k <= kxmax; k++) {
    for (l = 1; l <= kymax; l++) {
      for (m = 1; m <= kzmax; m++) {
	sqk = (k*unitk[0] * k*unitk[0]) + (l*unitk[1] * l*unitk[1]) +
	  (m*unitk[2] * m*unitk[2]);
	if (sqk <= gsqmx) {
	  cstr1 = 0.0;
	  sstr1 = 0.0;
	  cstr2 = 0.0;
	  sstr2 = 0.0;
	  cstr3 = 0.0;
	  sstr3 = 0.0;
	  cstr4 = 0.0;
	  sstr4 = 0.0;
	  for (i = 0; i < nlocal; i++) {
	    clpm = cs[l][1][i]*cs[m][2][i] - sn[l][1][i]*sn[m][2][i];
	    slpm = sn[l][1][i]*cs[m][2][i] + cs[l][1][i]*sn[m][2][i];
	    cstr1 += q[i]*(cs[k][0][i]*clpm - sn[k][0][i]*slpm);
	    sstr1 += q[i]*(sn[k][0][i]*clpm + cs[k][0][i]*slpm);
	    
	    clpm = cs[l][1][i]*cs[m][2][i] + sn[l][1][i]*sn[m][2][i];
	    slpm = -sn[l][1][i]*cs[m][2][i] + cs[l][1][i]*sn[m][2][i];
	    cstr2 += q[i]*(cs[k][0][i]*clpm - sn[k][0][i]*slpm);
	    sstr2 += q[i]*(sn[k][0][i]*clpm + cs[k][0][i]*slpm);
	    
	    clpm = cs[l][1][i]*cs[m][2][i] + sn[l][1][i]*sn[m][2][i];
	    slpm = sn[l][1][i]*cs[m][2][i] - cs[l][1][i]*sn[m][2][i];
	    cstr3 += q[i]*(cs[k][0][i]*clpm - sn[k][0][i]*slpm);
	    sstr3 += q[i]*(sn[k][0][i]*clpm + cs[k][0][i]*slpm);
	    
	    clpm = cs[l][1][i]*cs[m][2][i] - sn[l][1][i]*sn[m][2][i];
	    slpm = -sn[l][1][i]*cs[m][2][i] - cs[l][1][i]*sn[m][2][i];
	    cstr4 += q[i]*(cs[k][0][i]*clpm - sn[k][0][i]*slpm);
	    sstr4 += q[i]*(sn[k][0][i]*clpm + cs[k][0][i]*slpm);
	  }
	  sfacrl[n] = cstr1;
	  sfacim[n++] = sstr1;
	  sfacrl[n] = cstr2;
	  sfacim[n++] = sstr2;
	  sfacrl[n] = cstr3;
	  sfacim[n++] = sstr3;
	  sfacrl[n] = cstr4;
	  sfacim[n++] = sstr4;
	}
      }
    }
  }
}

/* ----------------------------------------------------------------------
   pre-compute coefficients for each Ewald K-vector 
------------------------------------------------------------------------- */

void Ewald::coeffs()
{
  int k,l,m;
  double sqk,vterm;

  double g_ewald_sq_inv = 1.0 / (g_ewald*g_ewald);
  double preu = 4.0*MY_PI/volume;

  kcount = 0;

  // (k,0,0), (0,l,0), (0,0,m)

  for (m = 1; m <= kmax; m++) {
    sqk = (m*unitk[0]) * (m*unitk[0]);
    if (sqk <= gsqmx) {
      kxvecs[kcount] = m;
      kyvecs[kcount] = 0;
      kzvecs[kcount] = 0;
      ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
      eg[kcount][0] = 2.0*unitk[0]*m*ug[kcount];
      eg[kcount][1] = 0.0;
      eg[kcount][2] = 0.0;
      vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
      vg[kcount][0] = 1.0 + vterm*(unitk[0]*m)*(unitk[0]*m);
      vg[kcount][1] = 1.0;
      vg[kcount][2] = 1.0;
      vg[kcount][3] = 0.0;
      vg[kcount][4] = 0.0;
      vg[kcount][5] = 0.0;
      kcount++;
    }
    sqk = (m*unitk[1]) * (m*unitk[1]);
    if (sqk <= gsqmx) {
      kxvecs[kcount] = 0;
      kyvecs[kcount] = m;
      kzvecs[kcount] = 0;
      ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
      eg[kcount][0] = 0.0;
      eg[kcount][1] = 2.0*unitk[1]*m*ug[kcount];
      eg[kcount][2] = 0.0;
      vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
      vg[kcount][0] = 1.0;
      vg[kcount][1] = 1.0 + vterm*(unitk[1]*m)*(unitk[1]*m);
      vg[kcount][2] = 1.0;
      vg[kcount][3] = 0.0;
      vg[kcount][4] = 0.0;
      vg[kcount][5] = 0.0;
      kcount++;
    }
    sqk = (m*unitk[2]) * (m*unitk[2]);
    if (sqk <= gsqmx) {
      kxvecs[kcount] = 0;
      kyvecs[kcount] = 0;
      kzvecs[kcount] = m;
      ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
      eg[kcount][0] = 0.0;
      eg[kcount][1] = 0.0;
      eg[kcount][2] = 2.0*unitk[2]*m*ug[kcount];
      vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
      vg[kcount][0] = 1.0;
      vg[kcount][1] = 1.0;
      vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
      vg[kcount][3] = 0.0;
      vg[kcount][4] = 0.0;
      vg[kcount][5] = 0.0;
      kcount++;
    }
  }

  // 1 = (k,l,0), 2 = (k,-l,0)

  for (k = 1; k <= kxmax; k++) {
    for (l = 1; l <= kymax; l++) {
      sqk = (unitk[0]*k) * (unitk[0]*k) + (unitk[1]*l) * (unitk[1]*l);
      if (sqk <= gsqmx) {
	kxvecs[kcount] = k;
	kyvecs[kcount] = l;
	kzvecs[kcount] = 0;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	eg[kcount][1] = 2.0*unitk[1]*l*ug[kcount];
	eg[kcount][2] = 0.0;
	vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
	vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	vg[kcount][2] = 1.0;
	vg[kcount][3] = vterm*unitk[0]*k*unitk[1]*l;
	vg[kcount][4] = 0.0;
	vg[kcount][5] = 0.0;
	kcount++;

	kxvecs[kcount] = k;
	kyvecs[kcount] = -l;
	kzvecs[kcount] = 0;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	eg[kcount][1] = -2.0*unitk[1]*l*ug[kcount];
	eg[kcount][2] = 0.0;
	vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	vg[kcount][2] = 1.0;
	vg[kcount][3] = -vterm*unitk[0]*k*unitk[1]*l;
	vg[kcount][4] = 0.0;
	vg[kcount][5] = 0.0;
	kcount++;;
      }
    }
  }

  // 1 = (0,l,m), 2 = (0,l,-m)

  for (l = 1; l <= kymax; l++) {
    for (m = 1; m <= kzmax; m++) {
      sqk = (unitk[1]*l) * (unitk[1]*l) + (unitk[2]*m) * (unitk[2]*m);
      if (sqk <= gsqmx) {
	kxvecs[kcount] = 0;
	kyvecs[kcount] = l;
	kzvecs[kcount] = m;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] =  0.0;
	eg[kcount][1] =  2.0*unitk[1]*l*ug[kcount];
	eg[kcount][2] =  2.0*unitk[2]*m*ug[kcount];
	vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
	vg[kcount][0] = 1.0;
	vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	vg[kcount][3] = 0.0;
	vg[kcount][4] = 0.0;
	vg[kcount][5] = vterm*unitk[1]*l*unitk[2]*m;
	kcount++;

	kxvecs[kcount] = 0;
	kyvecs[kcount] = l;
	kzvecs[kcount] = -m;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] =  0.0;
	eg[kcount][1] =  2.0*unitk[1]*l*ug[kcount];
	eg[kcount][2] = -2.0*unitk[2]*m*ug[kcount];
	vg[kcount][0] = 1.0;
	vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	vg[kcount][3] = 0.0;
	vg[kcount][4] = 0.0;
	vg[kcount][5] = -vterm*unitk[1]*l*unitk[2]*m;
	kcount++;
      }
    }
  }

  // 1 = (k,0,m), 2 = (k,0,-m)

  for (k = 1; k <= kxmax; k++) {
    for (m = 1; m <= kzmax; m++) {
      sqk = (unitk[0]*k) * (unitk[0]*k) + (unitk[2]*m) * (unitk[2]*m);
      if (sqk <= gsqmx) {
	kxvecs[kcount] = k;
	kyvecs[kcount] = 0;
	kzvecs[kcount] = m;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] =  2.0*unitk[0]*k*ug[kcount];
	eg[kcount][1] =  0.0;
	eg[kcount][2] =  2.0*unitk[2]*m*ug[kcount];
	vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
	vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	vg[kcount][1] = 1.0;
	vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	vg[kcount][3] = 0.0;
	vg[kcount][4] = vterm*unitk[0]*k*unitk[2]*m;
	vg[kcount][5] = 0.0;
	kcount++;

	kxvecs[kcount] = k;
	kyvecs[kcount] = 0;
	kzvecs[kcount] = -m;
	ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	eg[kcount][0] =  2.0*unitk[0]*k*ug[kcount];
	eg[kcount][1] =  0.0;
	eg[kcount][2] = -2.0*unitk[2]*m*ug[kcount];
	vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	vg[kcount][1] = 1.0;
	vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	vg[kcount][3] = 0.0;
	vg[kcount][4] = -vterm*unitk[0]*k*unitk[2]*m;
	vg[kcount][5] = 0.0;
	kcount++;
      }
    }
  }

  // 1 = (k,l,m), 2 = (k,-l,m), 3 = (k,l,-m), 4 = (k,-l,-m)

  for (k = 1; k <= kxmax; k++) {
    for (l = 1; l <= kymax; l++) {
      for (m = 1; m <= kzmax; m++) {
	sqk = (unitk[0]*k) * (unitk[0]*k) + (unitk[1]*l) * (unitk[1]*l) + 
	  (unitk[2]*m) * (unitk[2]*m);
	if (sqk <= gsqmx) {
	  kxvecs[kcount] = k;
	  kyvecs[kcount] = l;
	  kzvecs[kcount] = m;
	  ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	  eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	  eg[kcount][1] = 2.0*unitk[1]*l*ug[kcount];
	  eg[kcount][2] = 2.0*unitk[2]*m*ug[kcount];
	  vterm = -2.0*(1.0/sqk + 0.25*g_ewald_sq_inv);
	  vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	  vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	  vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	  vg[kcount][3] = vterm*unitk[0]*k*unitk[1]*l;
	  vg[kcount][4] = vterm*unitk[0]*k*unitk[2]*m;
	  vg[kcount][5] = vterm*unitk[1]*l*unitk[2]*m;
	  kcount++;

	  kxvecs[kcount] = k;
	  kyvecs[kcount] = -l;
	  kzvecs[kcount] = m;
	  ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	  eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	  eg[kcount][1] = -2.0*unitk[1]*l*ug[kcount];
	  eg[kcount][2] = 2.0*unitk[2]*m*ug[kcount];
	  vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	  vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	  vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	  vg[kcount][3] = -vterm*unitk[0]*k*unitk[1]*l;
	  vg[kcount][4] = vterm*unitk[0]*k*unitk[2]*m;
	  vg[kcount][5] = -vterm*unitk[1]*l*unitk[2]*m;
	  kcount++;

	  kxvecs[kcount] = k;
	  kyvecs[kcount] = l;
	  kzvecs[kcount] = -m;
	  ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	  eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	  eg[kcount][1] = 2.0*unitk[1]*l*ug[kcount];
	  eg[kcount][2] = -2.0*unitk[2]*m*ug[kcount];
	  vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	  vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	  vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	  vg[kcount][3] = vterm*unitk[0]*k*unitk[1]*l;
	  vg[kcount][4] = -vterm*unitk[0]*k*unitk[2]*m;
	  vg[kcount][5] = -vterm*unitk[1]*l*unitk[2]*m;
	  kcount++;

	  kxvecs[kcount] = k;
	  kyvecs[kcount] = -l;
	  kzvecs[kcount] = -m;
	  ug[kcount] = preu*exp(-0.25*sqk*g_ewald_sq_inv)/sqk;
	  eg[kcount][0] = 2.0*unitk[0]*k*ug[kcount];
	  eg[kcount][1] = -2.0*unitk[1]*l*ug[kcount];
	  eg[kcount][2] = -2.0*unitk[2]*m*ug[kcount];
	  vg[kcount][0] = 1.0 + vterm*(unitk[0]*k)*(unitk[0]*k);
	  vg[kcount][1] = 1.0 + vterm*(unitk[1]*l)*(unitk[1]*l);
	  vg[kcount][2] = 1.0 + vterm*(unitk[2]*m)*(unitk[2]*m);
	  vg[kcount][3] = -vterm*unitk[0]*k*unitk[1]*l;
	  vg[kcount][4] = -vterm*unitk[0]*k*unitk[2]*m;
	  vg[kcount][5] = vterm*unitk[1]*l*unitk[2]*m;
	  kcount++;
	}
      }
    }
  }
}

/* ----------------------------------------------------------------------
   allocate memory that depends on # of K-vectors 
------------------------------------------------------------------------- */

void Ewald::allocate()
{
  kxvecs = new int[kmax3d];
  kyvecs = new int[kmax3d];
  kzvecs = new int[kmax3d];

  ug = new double[kmax3d];
  memory->create(eg,kmax3d,3,"ewald:eg");
  memory->create(vg,kmax3d,6,"ewald:vg");

  sfacrl = new double[kmax3d];
  sfacim = new double[kmax3d];
  sfacrl_all = new double[kmax3d];
  sfacim_all = new double[kmax3d];
}

/* ----------------------------------------------------------------------
   deallocate memory that depends on # of K-vectors 
------------------------------------------------------------------------- */

void Ewald::deallocate()
{
  delete [] kxvecs;
  delete [] kyvecs;
  delete [] kzvecs;
  
  delete [] ug;
  memory->destroy(eg);
  memory->destroy(vg);

  delete [] sfacrl;
  delete [] sfacim;
  delete [] sfacrl_all;
  delete [] sfacim_all;
}

/* ----------------------------------------------------------------------
   Slab-geometry correction term to dampen inter-slab interactions between
   periodically repeating slabs.  Yields good approximation to 2-D Ewald if 
   adequate empty space is left between repeating slabs (J. Chem. Phys. 
   111, 3155).  Slabs defined here to be parallel to the xy plane. 
------------------------------------------------------------------------- */

void Ewald::slabcorr()
{
  // compute local contribution to global dipole moment
  
  double *q = atom->q;
  double **x = atom->x;
  int nlocal = atom->nlocal;

  double dipole = 0.0;
  for (int i = 0; i < nlocal; i++) dipole += q[i]*x[i][2];
  
  // sum local contributions to get global dipole moment

  double dipole_all;
  MPI_Allreduce(&dipole,&dipole_all,1,MPI_DOUBLE,MPI_SUM,world);

  // compute corrections
  
  const double e_slabcorr = 2.0*MY_PI*dipole_all*dipole_all/volume;
  const double qscale = force->qqrd2e * scale;
  
  if (eflag_global) energy += qscale * e_slabcorr;
  
  // per-atom energy
    
  if (eflag_atom) {
    double efact = 2.0*MY_PI*dipole_all/volume; 
    for (int i = 0; i < nlocal; i++) eatom[i] += qscale * q[i]*x[i][2]*efact;
  }

  // add on force corrections

  double ffact = -4.0*MY_PI*dipole_all/volume; 
  double **f = atom->f;

  for (int i = 0; i < nlocal; i++) f[i][2] += qscale * q[i]*ffact;
}

/* ----------------------------------------------------------------------
   memory usage of local arrays 
------------------------------------------------------------------------- */

double Ewald::memory_usage()
{
  double bytes = 3 * kmax3d * sizeof(int);
  bytes += (1 + 3 + 6) * kmax3d * sizeof(double);
  bytes += 4 * kmax3d * sizeof(double);
  bytes += nmax*3 * sizeof(double);
  bytes += 2 * (2*kmax+1)*3*nmax * sizeof(double);
  return bytes;
}

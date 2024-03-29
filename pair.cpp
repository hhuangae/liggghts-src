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
   Contributing author: Paul Crozier (SNL)
------------------------------------------------------------------------- */

#include "mpi.h"
#include "float.h"
#include "limits.h"
#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair.h"
#include "atom.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "domain.h"
#include "comm.h"
#include "force.h"
#include "update.h"
#include "accelerator_cuda.h"
#include "memory.h"
#include "error.h"
#include "suffix.h"

using namespace LAMMPS_NS;

enum{GEOMETRIC,ARITHMETIC,SIXTHPOWER};
enum{R,RSQ,BMP};

/* ---------------------------------------------------------------------- */

Pair::Pair(LAMMPS *lmp) : Pointers(lmp)
{
  THIRD = 1.0/3.0;

  eng_vdwl = eng_coul = 0.0;

  comm_forward = comm_reverse = comm_reverse_off = 0;

  single_enable = 1;
  restartinfo = 1;
  respa_enable = 0;
  one_coeff = 0;
  no_virial_fdotr_compute = 0;
  ghostneigh = 0;

  nextra = 0;
  pvector = NULL;
  single_extra = 0;
  svector = NULL;

  // pair_modify settings

  offset_flag = 0;
  mix_flag = GEOMETRIC;
  tail_flag = 0;
  etail = ptail = etail_ij = ptail_ij = 0.0;
  ncoultablebits = 12;
  tabinner = sqrt(2.0);

  allocated = 0;
  suffix_flag = Suffix::NONE;

  maxeatom = maxvatom = 0;
  eatom = NULL;
  vatom = NULL;
}

/* ---------------------------------------------------------------------- */

Pair::~Pair()
{
  memory->destroy(eatom);
  memory->destroy(vatom);
}

/* ----------------------------------------------------------------------
   modify parameters of the pair style
   pair_hybrid has its own version of this routine for its sub-styles
------------------------------------------------------------------------- */

void Pair::modify_params(int narg, char **arg)
{
  if (narg == 0) error->all(FLERR,"Illegal pair_modify command");

  int iarg = 0;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"mix") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_modify command");
      if (strcmp(arg[iarg+1],"geometric") == 0) mix_flag = GEOMETRIC;
      else if (strcmp(arg[iarg+1],"arithmetic") == 0) mix_flag = ARITHMETIC;
      else if (strcmp(arg[iarg+1],"sixthpower") == 0) mix_flag = SIXTHPOWER;
      else error->all(FLERR,"Illegal pair_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"shift") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) offset_flag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) offset_flag = 0;
      else error->all(FLERR,"Illegal pair_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"table") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_modify command");
      ncoultablebits = atoi(arg[iarg+1]);
      if (ncoultablebits > sizeof(float)*CHAR_BIT) 
        error->all(FLERR,"Too many total bits for bitmapped lookup table");
      iarg += 2;
    } else if (strcmp(arg[iarg],"tabinner") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_modify command");
      tabinner = atof(arg[iarg+1]);
      iarg += 2;
    } else if (strcmp(arg[iarg],"tail") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) tail_flag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) tail_flag = 0;
      else error->all(FLERR,"Illegal pair_modify command");
      iarg += 2;
    } else error->all(FLERR,"Illegal pair_modify command");
  }
}

/* ---------------------------------------------------------------------- */

void Pair::init()
{
  int i,j;

  if (offset_flag && tail_flag)
    error->all(FLERR,"Cannot have both pair_modify shift and tail set to yes");
  if (tail_flag && domain->dimension == 2)
    error->all(FLERR,"Cannot use pair tail corrections with 2d simulations");
  if (tail_flag && domain->nonperiodic && comm->me == 0)
    error->warning(FLERR,"Using pair tail corrections with nonperiodic system");

  if (!allocated) error->all(FLERR,"All pair coeffs are not set");

  // I,I coeffs must be set
  // init_one() will check if I,J is set explicitly or inferred by mixing

  for (i = 1; i <= atom->ntypes; i++)
    if (setflag[i][i] == 0) error->all(FLERR,"All pair coeffs are not set");

  // style-specific initialization

  init_style();

  // call init_one() for each I,J
  // set cutsq for each I,J, used to neighbor
  // cutforce = max of all I,J cutoffs

  cutforce = 0.0;
  etail = ptail = 0.0;
  double cut;

  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      cut = init_one(i,j);
      cutsq[i][j] = cutsq[j][i] = cut*cut;
      cutforce = MAX(cutforce,cut);
      if (tail_flag) {
	etail += etail_ij;
	ptail += ptail_ij;
	if (i != j) {
	  etail += etail_ij;
	  ptail += ptail_ij;
	}
      }
    }
}

/* ----------------------------------------------------------------------
   reset all type-based params by invoking init_one() for each I,J
   called by fix adapt after it changes one or more params
------------------------------------------------------------------------- */

void Pair::reinit()
{
  int i,j;
  double tmp;

  etail = ptail = 0.0;

  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      tmp = init_one(i,j);
      if (tail_flag) {
	etail += etail_ij;
	ptail += ptail_ij;
	if (i != j) {
	  etail += etail_ij;
	  ptail += ptail_ij;
	}
      }
    }
}

/* ----------------------------------------------------------------------
   init specific to a pair style
   specific pair style can override this function
     if needs its own error checks
     if needs another kind of neighbor list
   request default neighbor list = half list
------------------------------------------------------------------------- */

void Pair::init_style()
{
  neighbor->request(this);
}

/* ----------------------------------------------------------------------
   neighbor callback to inform pair style of neighbor list to use
   specific pair style can override this function
------------------------------------------------------------------------- */

void Pair::init_list(int which, NeighList *ptr)
{
  list = ptr;
}

/* ----------------------------------------------------------------------
   mixing of pair potential prefactors (epsilon)
------------------------------------------------------------------------- */

double Pair::mix_energy(double eps1, double eps2, double sig1, double sig2)
{
  double value;
  if (mix_flag == GEOMETRIC)
    value = sqrt(eps1*eps2);
  else if (mix_flag == ARITHMETIC)
    value = sqrt(eps1*eps2);
  else if (mix_flag == SIXTHPOWER)
    value = 2.0 * sqrt(eps1*eps2) *
      pow(sig1,3.0) * pow(sig2,3.0) / (pow(sig1,6.0) + pow(sig2,6.0));
  return value;
}

/* ----------------------------------------------------------------------
   mixing of pair potential distances (sigma, cutoff)
------------------------------------------------------------------------- */

double Pair::mix_distance(double sig1, double sig2)
{
  double value;
  if (mix_flag == GEOMETRIC)
    value = sqrt(sig1*sig2);
  else if (mix_flag == ARITHMETIC)
    value = 0.5 * (sig1+sig2);
  else if (mix_flag == SIXTHPOWER)
    value = pow((0.5 * (pow(sig1,6.0) + pow(sig2,6.0))),1.0/6.0);
  return value;
}

/* ----------------------------------------------------------------------
   setup for energy, virial computation
   see integrate::ev_set() for values of eflag (0-3) and vflag (0-6)
------------------------------------------------------------------------- */

void Pair::ev_setup(int eflag, int vflag)
{
  int i,n;

  evflag = 1;

  eflag_either = eflag;
  eflag_global = eflag % 2;
  eflag_atom = eflag / 2;

  vflag_either = vflag;
  vflag_global = vflag % 4;
  vflag_atom = vflag / 4;

  // reallocate per-atom arrays if necessary

  if (eflag_atom && atom->nmax > maxeatom) {
    maxeatom = atom->nmax;
    memory->destroy(eatom);
    memory->create(eatom,comm->nthreads*maxeatom,"pair:eatom");
  }
  if (vflag_atom && atom->nmax > maxvatom) {
    maxvatom = atom->nmax;
    memory->destroy(vatom);
    memory->create(vatom,comm->nthreads*maxvatom,6,"pair:vatom");
  }

  // zero accumulators
  // use force->newton instead of newton_pair
  //   b/c some bonds/dihedrals call pair::ev_tally with pairwise info

  if (eflag_global) eng_vdwl = eng_coul = 0.0;
  if (vflag_global) for (i = 0; i < 6; i++) virial[i] = 0.0;
  if (eflag_atom) {
    n = atom->nlocal;
    if (force->newton) n += atom->nghost;
    for (i = 0; i < n; i++) eatom[i] = 0.0;
  }
  if (vflag_atom) {
    n = atom->nlocal;
    if (force->newton) n += atom->nghost;
    for (i = 0; i < n; i++) {
      vatom[i][0] = 0.0;
      vatom[i][1] = 0.0;
      vatom[i][2] = 0.0;
      vatom[i][3] = 0.0;
      vatom[i][4] = 0.0;
      vatom[i][5] = 0.0;
    }
  }

  // if vflag_global = 2 and pair::compute() calls virial_fdotr_compute()
  // compute global virial via (F dot r) instead of via pairwise summation
  // unset other flags as appropriate

  if (vflag_global == 2 && no_virial_fdotr_compute == 0) {
    vflag_fdotr = 1;
    vflag_global = 0;
    if (vflag_atom == 0) vflag_either = 0;
    if (vflag_either == 0 && eflag_either == 0) evflag = 0;
  } else vflag_fdotr = 0;

  if (lmp->cuda) lmp->cuda->evsetup_eatom_vatom(eflag_atom,vflag_atom);
}

/* ----------------------------------------------------------------------
   tally eng_vdwl and virial into global and per-atom accumulators
   need i < nlocal test since called by bond_quartic and dihedral_charmm
------------------------------------------------------------------------- */

void Pair::ev_tally(int i, int j, int nlocal, int newton_pair,
		    double evdwl, double ecoul, double fpair,
		    double delx, double dely, double delz)
{
  double evdwlhalf,ecoulhalf,epairhalf,v[6];

  if (eflag_either) {
    if (eflag_global) {
      if (newton_pair) {
	eng_vdwl += evdwl;
	eng_coul += ecoul;
      } else {
	evdwlhalf = 0.5*evdwl;
	ecoulhalf = 0.5*ecoul;
	if (i < nlocal) {
	  eng_vdwl += evdwlhalf;
	  eng_coul += ecoulhalf;
	}
	if (j < nlocal) {
	  eng_vdwl += evdwlhalf;
	  eng_coul += ecoulhalf;
	}
      }
    }
    if (eflag_atom) {
      epairhalf = 0.5 * (evdwl + ecoul);
      if (newton_pair || i < nlocal) eatom[i] += epairhalf;
      if (newton_pair || j < nlocal) eatom[j] += epairhalf;
    }
  }

  if (vflag_either) {
    v[0] = delx*delx*fpair;
    v[1] = dely*dely*fpair;
    v[2] = delz*delz*fpair;
    v[3] = delx*dely*fpair;
    v[4] = delx*delz*fpair;
    v[5] = dely*delz*fpair;

    if (vflag_global) {
      if (newton_pair) {
	virial[0] += v[0];
	virial[1] += v[1];
	virial[2] += v[2];
	virial[3] += v[3];
	virial[4] += v[4];
	virial[5] += v[5];
      } else {
	if (i < nlocal) {
	  virial[0] += 0.5*v[0];
	  virial[1] += 0.5*v[1];
	  virial[2] += 0.5*v[2];
	  virial[3] += 0.5*v[3];
	  virial[4] += 0.5*v[4];
	  virial[5] += 0.5*v[5];
	}
	if (j < nlocal) {
	  virial[0] += 0.5*v[0];
	  virial[1] += 0.5*v[1];
	  virial[2] += 0.5*v[2];
	  virial[3] += 0.5*v[3];
	  virial[4] += 0.5*v[4];
	  virial[5] += 0.5*v[5];
	}
      }
    }

    if (vflag_atom) {
      if (newton_pair || i < nlocal) {
	vatom[i][0] += 0.5*v[0];
	vatom[i][1] += 0.5*v[1];
	vatom[i][2] += 0.5*v[2];
	vatom[i][3] += 0.5*v[3];
	vatom[i][4] += 0.5*v[4];
	vatom[i][5] += 0.5*v[5];
      }
      if (newton_pair || j < nlocal) {
	vatom[j][0] += 0.5*v[0];
	vatom[j][1] += 0.5*v[1];
	vatom[j][2] += 0.5*v[2];
	vatom[j][3] += 0.5*v[3];
	vatom[j][4] += 0.5*v[4];
	vatom[j][5] += 0.5*v[5];
      }
    }
  }
}

/* ---------------------------------------------------------------------- 
   tally eng_vdwl and virial into global and per-atom accumulators
   can use this version with full neighbor lists
------------------------------------------------------------------------- */

void Pair::ev_tally_full(int i, double evdwl, double ecoul, double fpair, 
                         double delx, double dely, double delz)
{
  double v[6];

  if (eflag_either) {
    if (eflag_global) {
      eng_vdwl += 0.5*evdwl;
      eng_coul += 0.5*ecoul;
    }
    if (eflag_atom) eatom[i] += 0.5 * (evdwl + ecoul);
  }

  if (vflag_either) {
    v[0] = 0.5*delx*delx*fpair;
    v[1] = 0.5*dely*dely*fpair;
    v[2] = 0.5*delz*delz*fpair;
    v[3] = 0.5*delx*dely*fpair;
    v[4] = 0.5*delx*delz*fpair;
    v[5] = 0.5*dely*delz*fpair;

    if (vflag_global) {
      virial[0] += v[0];
      virial[1] += v[1];
      virial[2] += v[2];
      virial[3] += v[3];
      virial[4] += v[4];
      virial[5] += v[5];
    }

    if (vflag_atom) {
      vatom[i][0] += v[0];
      vatom[i][1] += v[1];
      vatom[i][2] += v[2];
      vatom[i][3] += v[3];
      vatom[i][4] += v[4];
      vatom[i][5] += v[5];
    }
  }
}

/* ----------------------------------------------------------------------
   tally eng_vdwl and virial into global and per-atom accumulators
   for virial, have delx,dely,delz and fx,fy,fz
------------------------------------------------------------------------- */

void Pair::ev_tally_xyz(int i, int j, int nlocal, int newton_pair,
			double evdwl, double ecoul,
			double fx, double fy, double fz,
			double delx, double dely, double delz)
{
  double evdwlhalf,ecoulhalf,epairhalf,v[6];
  
  if (eflag_either) {
    if (eflag_global) {
      if (newton_pair) {
	eng_vdwl += evdwl;
	eng_coul += ecoul;
      } else {
	evdwlhalf = 0.5*evdwl;
	ecoulhalf = 0.5*ecoul;
	if (i < nlocal) {
	  eng_vdwl += evdwlhalf;
	  eng_coul += ecoulhalf;
	}
	if (j < nlocal) {
	  eng_vdwl += evdwlhalf;
	  eng_coul += ecoulhalf;
	}
      }
    }
    if (eflag_atom) {
      epairhalf = 0.5 * (evdwl + ecoul);
      if (newton_pair || i < nlocal) eatom[i] += epairhalf;
      if (newton_pair || j < nlocal) eatom[j] += epairhalf;
    }
  }

  if (vflag_either) {
    v[0] = delx*fx;
    v[1] = dely*fy;
    v[2] = delz*fz;
    v[3] = delx*fy;
    v[4] = delx*fz;
    v[5] = dely*fz;

    if (vflag_global) {
      if (newton_pair) {
	virial[0] += v[0];
	virial[1] += v[1];
	virial[2] += v[2];
	virial[3] += v[3];
	virial[4] += v[4];
	virial[5] += v[5];
      } else {
	if (i < nlocal) {
	  virial[0] += 0.5*v[0];
	  virial[1] += 0.5*v[1];
	  virial[2] += 0.5*v[2];
	  virial[3] += 0.5*v[3];
	  virial[4] += 0.5*v[4];
	  virial[5] += 0.5*v[5];
	}
	if (j < nlocal) {
	  virial[0] += 0.5*v[0];
	  virial[1] += 0.5*v[1];
	  virial[2] += 0.5*v[2];
	  virial[3] += 0.5*v[3];
	  virial[4] += 0.5*v[4];
	  virial[5] += 0.5*v[5];
	}
      }
    }

    if (vflag_atom) {
      if (newton_pair || i < nlocal) {
	vatom[i][0] += 0.5*v[0];
	vatom[i][1] += 0.5*v[1];
	vatom[i][2] += 0.5*v[2];
	vatom[i][3] += 0.5*v[3];
	vatom[i][4] += 0.5*v[4];
	vatom[i][5] += 0.5*v[5];
      }
      if (newton_pair || j < nlocal) {
	vatom[j][0] += 0.5*v[0];
	vatom[j][1] += 0.5*v[1];
	vatom[j][2] += 0.5*v[2];
	vatom[j][3] += 0.5*v[3];
	vatom[j][4] += 0.5*v[4];
	vatom[j][5] += 0.5*v[5];
      }
    }
  }
}

/* ----------------------------------------------------------------------
   tally eng_vdwl and virial into global and per-atom accumulators
   for virial, have delx,dely,delz and fx,fy,fz
   called when using full neighbor lists
------------------------------------------------------------------------- */

void Pair::ev_tally_xyz_full(int i, double evdwl, double ecoul,
			     double fx, double fy, double fz,
			     double delx, double dely, double delz)
{
  double evdwlhalf,ecoulhalf,epairhalf,v[6];
  
  if (eflag_either) {
    if (eflag_global) {
      evdwlhalf = 0.5*evdwl;
      ecoulhalf = 0.5*ecoul;
      eng_vdwl += evdwlhalf;
      eng_coul += ecoulhalf;
    }
    if (eflag_atom) {
      epairhalf = 0.5 * (evdwl + ecoul);
      eatom[i] += epairhalf;
    }
  }

  if (vflag_either) {
    v[0] = 0.5*delx*fx;
    v[1] = 0.5*dely*fy;
    v[2] = 0.5*delz*fz;
    v[3] = 0.5*delx*fy;
    v[4] = 0.5*delx*fz;
    v[5] = 0.5*dely*fz;

    if (vflag_global) {
      virial[0] += v[0];
      virial[1] += v[1];
      virial[2] += v[2];
      virial[3] += v[3];
      virial[4] += v[4];
      virial[5] += v[5];
    }
    
    if (vflag_atom) {
      vatom[i][0] += v[0];
      vatom[i][1] += v[1];
      vatom[i][2] += v[2];
      vatom[i][3] += v[3];
      vatom[i][4] += v[4];
      vatom[i][5] += v[5];
    }
  }
}

/* ----------------------------------------------------------------------
   tally eng_vdwl and virial into global and per-atom accumulators
   called by SW and hbond potentials, newton_pair is always on
   virial = riFi + rjFj + rkFk = (rj-ri) Fj + (rk-ri) Fk = drji*fj + drki*fk
 ------------------------------------------------------------------------- */

void Pair::ev_tally3(int i, int j, int k, double evdwl, double ecoul,
		     double *fj, double *fk, double *drji, double *drki)
{
  double epairthird,v[6];

  if (eflag_either) {
    if (eflag_global) {
      eng_vdwl += evdwl;
      eng_coul += ecoul;
    }
    if (eflag_atom) {
      epairthird = THIRD * (evdwl + ecoul);
      eatom[i] += epairthird;
      eatom[j] += epairthird;
      eatom[k] += epairthird;
    }
  }

  if (vflag_either) {
    v[0] = drji[0]*fj[0] + drki[0]*fk[0];
    v[1] = drji[1]*fj[1] + drki[1]*fk[1];
    v[2] = drji[2]*fj[2] + drki[2]*fk[2];
    v[3] = drji[0]*fj[1] + drki[0]*fk[1];
    v[4] = drji[0]*fj[2] + drki[0]*fk[2];
    v[5] = drji[1]*fj[2] + drki[1]*fk[2];
      
    if (vflag_global) {
      virial[0] += v[0];
      virial[1] += v[1];
      virial[2] += v[2];
      virial[3] += v[3];
      virial[4] += v[4];
      virial[5] += v[5];
    }

    if (vflag_atom) {
      vatom[i][0] += THIRD*v[0]; vatom[i][1] += THIRD*v[1];
      vatom[i][2] += THIRD*v[2]; vatom[i][3] += THIRD*v[3]; 
      vatom[i][4] += THIRD*v[4]; vatom[i][5] += THIRD*v[5];

      vatom[j][0] += THIRD*v[0]; vatom[j][1] += THIRD*v[1];
      vatom[j][2] += THIRD*v[2]; vatom[j][3] += THIRD*v[3];
      vatom[j][4] += THIRD*v[4]; vatom[j][5] += THIRD*v[5];

      vatom[k][0] += THIRD*v[0]; vatom[k][1] += THIRD*v[1];
      vatom[k][2] += THIRD*v[2]; vatom[k][3] += THIRD*v[3];
      vatom[k][4] += THIRD*v[4]; vatom[k][5] += THIRD*v[5];
    }
  }
}

/* ----------------------------------------------------------------------
   tally eng_vdwl and virial into global and per-atom accumulators
   called by AIREBO potential, newton_pair is always on
 ------------------------------------------------------------------------- */

void Pair::ev_tally4(int i, int j, int k, int m, double evdwl,
		     double *fi, double *fj, double *fk,
		     double *drim, double *drjm, double *drkm)
{
  double epairfourth,v[6];

  if (eflag_either) {
    if (eflag_global) eng_vdwl += evdwl;
    if (eflag_atom) {
      epairfourth = 0.25 * evdwl;
      eatom[i] += epairfourth;
      eatom[j] += epairfourth;
      eatom[k] += epairfourth;
      eatom[m] += epairfourth;
    }
  }

  if (vflag_atom) {
    v[0] = 0.25 * (drim[0]*fi[0] + drjm[0]*fj[0] + drkm[0]*fk[0]);
    v[1] = 0.25 * (drim[1]*fi[1] + drjm[1]*fj[1] + drkm[1]*fk[1]);
    v[2] = 0.25 * (drim[2]*fi[2] + drjm[2]*fj[2] + drkm[2]*fk[2]);
    v[3] = 0.25 * (drim[0]*fi[1] + drjm[0]*fj[1] + drkm[0]*fk[1]);
    v[4] = 0.25 * (drim[0]*fi[2] + drjm[0]*fj[2] + drkm[0]*fk[2]);
    v[5] = 0.25 * (drim[1]*fi[2] + drjm[1]*fj[2] + drkm[1]*fk[2]);
    
    vatom[i][0] += v[0]; vatom[i][1] += v[1]; vatom[i][2] += v[2];
    vatom[i][3] += v[3]; vatom[i][4] += v[4]; vatom[i][5] += v[5];
    vatom[j][0] += v[0]; vatom[j][1] += v[1]; vatom[j][2] += v[2];
    vatom[j][3] += v[3]; vatom[j][4] += v[4]; vatom[j][5] += v[5];
    vatom[k][0] += v[0]; vatom[k][1] += v[1]; vatom[k][2] += v[2];
    vatom[k][3] += v[3]; vatom[k][4] += v[4]; vatom[k][5] += v[5];
    vatom[m][0] += v[0]; vatom[m][1] += v[1]; vatom[m][2] += v[2];
    vatom[m][3] += v[3]; vatom[m][4] += v[4]; vatom[m][5] += v[5];
  }
}

/* ----------------------------------------------------------------------
   tally ecoul and virial into each of n atoms in list
   called by TIP4P potential, newton_pair is always on
   changes v values by dividing by n
 ------------------------------------------------------------------------- */

void Pair::ev_tally_list(int n, int *list, double ecoul, double *v)
{
  int i,j;

  if (eflag_either) {
    if (eflag_global) eng_coul += ecoul;
    if (eflag_atom) {
      double epairatom = ecoul/n;
      for (i = 0; i < n; i++) eatom[list[i]] += epairatom;
    }
  }

  if (vflag_either) {
    if (vflag_global) {
      virial[0] += v[0];
      virial[1] += v[1];
      virial[2] += v[2];
      virial[3] += v[3];
      virial[4] += v[4];
      virial[5] += v[5];
    }

    if (vflag_atom) {
      v[0] /= n;
      v[1] /= n;
      v[2] /= n;
      v[3] /= n;
      v[4] /= n;
      v[5] /= n;
      for (i = 0; i < n; i++) {
	j = list[i];
	vatom[j][0] += v[0];
	vatom[j][1] += v[1];
	vatom[j][2] += v[2];
	vatom[j][3] += v[3];
	vatom[j][4] += v[4];
	vatom[j][5] += v[5];
      }
    }
  }
}

/* ----------------------------------------------------------------------
   tally virial into per-atom accumulators
   called by REAX/C potential, newton_pair is always on
   fi is magnitude of force on atom i
------------------------------------------------------------------------- */

void Pair::v_tally(int i, double *fi)
{
  double v[6];
  double **x = atom->x;
  
  v[0] = x[i][0]*fi[0];
  v[1] = x[i][1]*fi[1];
  v[2] = x[i][2]*fi[2];
  v[3] = x[i][0]*fi[1];
  v[4] = x[i][0]*fi[2];
  v[5] = x[i][1]*fi[2];

  vatom[i][0] += v[0]; vatom[i][1] += v[1]; vatom[i][2] += v[2];
  vatom[i][3] += v[3]; vatom[i][4] += v[4]; vatom[i][5] += v[5];
}

/* ----------------------------------------------------------------------
   tally virial into per-atom accumulators
   called by AIREBO potential, newton_pair is always on
   fpair is magnitude of force on atom I
------------------------------------------------------------------------- */

void Pair::v_tally2(int i, int j, double fpair, double *drij)
{
  double v[6];
  
  v[0] = 0.5 * drij[0]*drij[0]*fpair;
  v[1] = 0.5 * drij[1]*drij[1]*fpair;
  v[2] = 0.5 * drij[2]*drij[2]*fpair;
  v[3] = 0.5 * drij[0]*drij[1]*fpair;
  v[4] = 0.5 * drij[0]*drij[2]*fpair;
  v[5] = 0.5 * drij[1]*drij[2]*fpair;

  vatom[i][0] += v[0]; vatom[i][1] += v[1]; vatom[i][2] += v[2];
  vatom[i][3] += v[3]; vatom[i][4] += v[4]; vatom[i][5] += v[5];
  vatom[j][0] += v[0]; vatom[j][1] += v[1]; vatom[j][2] += v[2];
  vatom[j][3] += v[3]; vatom[j][4] += v[4]; vatom[j][5] += v[5];
}

/* ----------------------------------------------------------------------
   tally virial into per-atom accumulators
   called by AIREBO and Tersoff potential, newton_pair is always on
------------------------------------------------------------------------- */

void Pair::v_tally3(int i, int j, int k,
		    double *fi, double *fj, double *drik, double *drjk)
{
  double v[6];
  
  v[0] = THIRD * (drik[0]*fi[0] + drjk[0]*fj[0]);
  v[1] = THIRD * (drik[1]*fi[1] + drjk[1]*fj[1]);
  v[2] = THIRD * (drik[2]*fi[2] + drjk[2]*fj[2]);
  v[3] = THIRD * (drik[0]*fi[1] + drjk[0]*fj[1]);
  v[4] = THIRD * (drik[0]*fi[2] + drjk[0]*fj[2]);
  v[5] = THIRD * (drik[1]*fi[2] + drjk[1]*fj[2]);

  vatom[i][0] += v[0]; vatom[i][1] += v[1]; vatom[i][2] += v[2];
  vatom[i][3] += v[3]; vatom[i][4] += v[4]; vatom[i][5] += v[5];
  vatom[j][0] += v[0]; vatom[j][1] += v[1]; vatom[j][2] += v[2];
  vatom[j][3] += v[3]; vatom[j][4] += v[4]; vatom[j][5] += v[5];
  vatom[k][0] += v[0]; vatom[k][1] += v[1]; vatom[k][2] += v[2];
  vatom[k][3] += v[3]; vatom[k][4] += v[4]; vatom[k][5] += v[5];
}

/* ----------------------------------------------------------------------
   tally virial into per-atom accumulators
   called by AIREBO potential, newton_pair is always on
------------------------------------------------------------------------- */

void Pair::v_tally4(int i, int j, int k, int m,
		    double *fi, double *fj, double *fk,
		    double *drim, double *drjm, double *drkm)
{
  double v[6];

  v[0] = 0.25 * (drim[0]*fi[0] + drjm[0]*fj[0] + drkm[0]*fk[0]);
  v[1] = 0.25 * (drim[1]*fi[1] + drjm[1]*fj[1] + drkm[1]*fk[1]);
  v[2] = 0.25 * (drim[2]*fi[2] + drjm[2]*fj[2] + drkm[2]*fk[2]);
  v[3] = 0.25 * (drim[0]*fi[1] + drjm[0]*fj[1] + drkm[0]*fk[1]);
  v[4] = 0.25 * (drim[0]*fi[2] + drjm[0]*fj[2] + drkm[0]*fk[2]);
  v[5] = 0.25 * (drim[1]*fi[2] + drjm[1]*fj[2] + drkm[1]*fk[2]);

  vatom[i][0] += v[0]; vatom[i][1] += v[1]; vatom[i][2] += v[2];
  vatom[i][3] += v[3]; vatom[i][4] += v[4]; vatom[i][5] += v[5];
  vatom[j][0] += v[0]; vatom[j][1] += v[1]; vatom[j][2] += v[2];
  vatom[j][3] += v[3]; vatom[j][4] += v[4]; vatom[j][5] += v[5];
  vatom[k][0] += v[0]; vatom[k][1] += v[1]; vatom[k][2] += v[2];
  vatom[k][3] += v[3]; vatom[k][4] += v[4]; vatom[k][5] += v[5];
  vatom[m][0] += v[0]; vatom[m][1] += v[1]; vatom[m][2] += v[2];
  vatom[m][3] += v[3]; vatom[m][4] += v[4]; vatom[m][5] += v[5];
}

/* ----------------------------------------------------------------------
   tally virial into global and per-atom accumulators
   called by pair lubricate potential with 6 tensor components
------------------------------------------------------------------------- */

void Pair::v_tally_tensor(int i, int j, int nlocal, int newton_pair,
			  double vxx, double vyy, double vzz,
			  double vxy, double vxz, double vyz)
{
  double v[6];

  v[0] = vxx;
  v[1] = vyy;
  v[2] = vzz;
  v[3] = vxy;
  v[4] = vxz;
  v[5] = vyz;
  
  if (vflag_global) {
    if (newton_pair) {
      virial[0] += v[0];
      virial[1] += v[1];
      virial[2] += v[2];
      virial[3] += v[3];
      virial[4] += v[4];
      virial[5] += v[5];
    } else {
      if (i < nlocal) {
	virial[0] += 0.5*v[0];
	virial[1] += 0.5*v[1];
	virial[2] += 0.5*v[2];
	virial[3] += 0.5*v[3];
	virial[4] += 0.5*v[4];
	virial[5] += 0.5*v[5];
      }
      if (j < nlocal) {
	virial[0] += 0.5*v[0];
	virial[1] += 0.5*v[1];
	virial[2] += 0.5*v[2];
	virial[3] += 0.5*v[3];
	virial[4] += 0.5*v[4];
	virial[5] += 0.5*v[5];
      }
    }
  }

  if (vflag_atom) {
    if (newton_pair || i < nlocal) {
      vatom[i][0] += 0.5*v[0];
      vatom[i][1] += 0.5*v[1];
      vatom[i][2] += 0.5*v[2];
      vatom[i][3] += 0.5*v[3];
      vatom[i][4] += 0.5*v[4];
      vatom[i][5] += 0.5*v[5];
    }
    if (newton_pair || j < nlocal) {
      vatom[j][0] += 0.5*v[0];
      vatom[j][1] += 0.5*v[1];
      vatom[j][2] += 0.5*v[2];
      vatom[j][3] += 0.5*v[3];
      vatom[j][4] += 0.5*v[4];
      vatom[j][5] += 0.5*v[5];
    }
  }
}

/* ----------------------------------------------------------------------
   compute global pair virial via summing F dot r over own & ghost atoms
   at this point, only pairwise forces have been accumulated in atom->f
------------------------------------------------------------------------- */

void Pair::virial_fdotr_compute()
{
  double **x = atom->x;
  double **f = atom->f;

  // sum over force on all particles including ghosts

  if (neighbor->includegroup == 0) {
    int nall = atom->nlocal + atom->nghost;
    for (int i = 0; i < nall; i++) {
      virial[0] += f[i][0]*x[i][0];
      virial[1] += f[i][1]*x[i][1];
      virial[2] += f[i][2]*x[i][2];
      virial[3] += f[i][1]*x[i][0];
      virial[4] += f[i][2]*x[i][0];
      virial[5] += f[i][2]*x[i][1];
    }

  // neighbor includegroup flag is set
  // sum over force on initial nfirst particles and ghosts

  } else {
    int nall = atom->nfirst;
    for (int i = 0; i < nall; i++) {
      virial[0] += f[i][0]*x[i][0];
      virial[1] += f[i][1]*x[i][1];
      virial[2] += f[i][2]*x[i][2];
      virial[3] += f[i][1]*x[i][0];
      virial[4] += f[i][2]*x[i][0];
      virial[5] += f[i][2]*x[i][1];
    }

    nall = atom->nlocal + atom->nghost;
    for (int i = atom->nlocal; i < nall; i++) {
      virial[0] += f[i][0]*x[i][0];
      virial[1] += f[i][1]*x[i][1];
      virial[2] += f[i][2]*x[i][2];
      virial[3] += f[i][1]*x[i][0];
      virial[4] += f[i][2]*x[i][0];
      virial[5] += f[i][2]*x[i][1];
    }
  }
}

/* ----------------------------------------------------------------------
   write a table of pair potential energy/force vs distance to a file
------------------------------------------------------------------------- */

void Pair::write_file(int narg, char **arg)
{
  if (narg < 8) error->all(FLERR,"Illegal pair_write command");
  if (single_enable == 0) 
    error->all(FLERR,"Pair style does not support pair_write");

  // parse arguments

  int itype = atoi(arg[0]);
  int jtype = atoi(arg[1]);
  if (itype < 1 || itype > atom->ntypes || jtype < 1 || jtype > atom->ntypes)
    error->all(FLERR,"Invalid atom types in pair_write command");

  int n = atoi(arg[2]);

  int style;
  if (strcmp(arg[3],"r") == 0) style = R;
  else if (strcmp(arg[3],"rsq") == 0) style = RSQ;
  else if (strcmp(arg[3],"bitmap") == 0) style = BMP;
  else error->all(FLERR,"Invalid style in pair_write command");

  double inner = atof(arg[4]);
  double outer = atof(arg[5]);
  if (inner <= 0.0 || inner >= outer)
    error->all(FLERR,"Invalid cutoffs in pair_write command");

  // open file in append mode
  // print header in format used by pair_style table

  int me;
  MPI_Comm_rank(world,&me);
  FILE *fp;
  if (me == 0) {
    fp = fopen(arg[6],"a");
    if (fp == NULL) error->one(FLERR,"Cannot open pair_write file");
    fprintf(fp,"# Pair potential %s for atom types %d %d: i,r,energy,force\n",
	    force->pair_style,itype,jtype);
    if (style == R) 
      fprintf(fp,"\n%s\nN %d R %g %g\n\n",arg[7],n,inner,outer);
    if (style == RSQ) 
      fprintf(fp,"\n%s\nN %d RSQ %g %g\n\n",arg[7],n,inner,outer);
  }

  // initialize potentials before evaluating pair potential
  // insures all pair coeffs are set and force constants

  force->init();

  // if pair style = any of EAM, swap in dummy fp vector

  double eamfp[2];
  eamfp[0] = eamfp[1] = 0.0;
  double *eamfp_hold;

  Pair *epair = force->pair_match("eam",0);
  if (epair) epair->swap_eam(eamfp,&eamfp_hold);

  // if atom style defines charge, swap in dummy q vec

  double q[2];
  q[0] = q[1] = 1.0;
  if (narg == 10) {
    q[0] = atof(arg[8]);
    q[1] = atof(arg[9]);
  }
  double *q_hold;

  if (atom->q) {
    q_hold = atom->q;
    atom->q = q;
  }

  // evaluate energy and force at each of N distances

  int masklo,maskhi,nmask,nshiftbits;
  if (style == BMP) {
    init_bitmap(inner,outer,n,masklo,maskhi,nmask,nshiftbits);
    int ntable = 1 << n;
    if (me == 0)
      fprintf(fp,"\n%s\nN %d BITMAP %g %g\n\n",arg[7],ntable,inner,outer);
    n = ntable;
  }

  double r,e,f,rsq;  
  union_int_float_t rsq_lookup;

  for (int i = 0; i < n; i++) {
    if (style == R) {
      r = inner + (outer-inner) * i/(n-1);
      rsq = r*r;
    } else if (style == RSQ) {
      rsq = inner*inner + (outer*outer - inner*inner) * i/(n-1);
      r = sqrt(rsq);
    } else if (style == BMP) {
      rsq_lookup.i = i << nshiftbits;
      rsq_lookup.i |= masklo;
      if (rsq_lookup.f < inner*inner) {
        rsq_lookup.i = i << nshiftbits;
        rsq_lookup.i |= maskhi;
      }
      rsq = rsq_lookup.f;
      r = sqrt(rsq);
    }

    if (rsq < cutsq[itype][jtype]) {
      e = single(0,1,itype,jtype,rsq,1.0,1.0,f);
      f *= r;
    } else e = f = 0.0;
    if (me == 0) fprintf(fp,"%d %g %g %g\n",i+1,r,e,f);
  }

  // restore original vecs that were swapped in for

  double *tmp;
  if (epair) epair->swap_eam(eamfp_hold,&tmp);
  if (atom->q) atom->q = q_hold;
  
  if (me == 0) fclose(fp);
}

/* ----------------------------------------------------------------------
   define bitmap parameters based on inner and outer cutoffs
------------------------------------------------------------------------- */

void Pair::init_bitmap(double inner, double outer, int ntablebits, 
             int &masklo, int &maskhi, int &nmask, int &nshiftbits)
{
  if (sizeof(int) != sizeof(float))
    error->all(FLERR,"Bitmapped lookup tables require int/float be same size");
  
  if (ntablebits > sizeof(float)*CHAR_BIT) 
    error->all(FLERR,"Too many total bits for bitmapped lookup table");
          
  if (inner >= outer) error->warning(FLERR,"Table inner cutoff >= outer cutoff");
    
  int nlowermin = 1;
  while (!((pow(double(2),nlowermin) <= inner*inner) && 
           (pow(double(2),nlowermin+1) > inner*inner))) {
    if (pow(double(2),nlowermin) <= inner*inner) nlowermin++;
    else nlowermin--;
  }

  int nexpbits = 0;
  double required_range = outer*outer / pow(double(2),nlowermin);
  double available_range = 2.0;
  
  while (available_range < required_range) {
    nexpbits++;
    available_range = pow(double(2),pow(double(2),nexpbits));
  }
     
  int nmantbits = ntablebits - nexpbits;

  if (nexpbits > sizeof(float)*CHAR_BIT - FLT_MANT_DIG) 
    error->all(FLERR,"Too many exponent bits for lookup table");
  if (nmantbits+1 > FLT_MANT_DIG)
    error->all(FLERR,"Too many mantissa bits for lookup table");
  if (nmantbits < 3) error->all(FLERR,"Too few bits for lookup table");

  nshiftbits = FLT_MANT_DIG - (nmantbits+1);

  nmask = 1;
  for (int j = 0; j < ntablebits+nshiftbits; j++) nmask *= 2;
  nmask -= 1;

  union_int_float_t rsq_lookup;
  rsq_lookup.f = outer*outer;
  maskhi = rsq_lookup.i & ~(nmask);
  rsq_lookup.f = inner*inner;
  masklo = rsq_lookup.i & ~(nmask);
}

/* ---------------------------------------------------------------------- */

double Pair::memory_usage()
{
  double bytes = comm->nthreads*maxeatom * sizeof(double);
  bytes += comm->nthreads*maxvatom*6 * sizeof(double);
  return bytes;
}

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

#ifndef LMP_MODIFY_H
#define LMP_MODIFY_H

#include "stdio.h"
#include "pointers.h"

namespace LAMMPS_NS {

class Modify : protected Pointers {
 public:
  int nfix,maxfix;
  int n_initial_integrate,n_post_integrate,n_pre_exchange,n_pre_neighbor;
  int n_pre_force,n_post_force;
  int n_final_integrate,n_end_of_step,n_thermo_energy;
  int n_initial_integrate_respa,n_post_integrate_respa;
  int n_pre_force_respa,n_post_force_respa,n_final_integrate_respa;
  int n_min_pre_exchange,n_min_pre_force,n_min_post_force,n_min_energy;

  int restart_pbc_any;       // 1 if any fix sets restart_pbc
  int nfix_restart_global;   // stored fix global info from restart file
  int nfix_restart_peratom;  // stored fix peratom info from restart file

  int allow_early_fix;       // 1 if allow fix creation at start of script

  class Fix **fix;           // list of fixes
  int *fmask;                // bit mask for when each fix is applied

  int ncompute,maxcompute;   // list of computes
  class Compute **compute;

  Modify(class LAMMPS *);
  virtual ~Modify();
  virtual void init();
  virtual void setup(int);
  virtual void setup_pre_exchange();
  virtual void setup_pre_force(int);
  virtual void initial_integrate(int);
  virtual void post_integrate();
  void pre_decide();
  virtual void pre_exchange();
  virtual void pre_neighbor();
  virtual void pre_force(int);
  virtual void post_force(int);
  virtual void final_integrate();
  virtual void end_of_step();
  virtual double thermo_energy();
  virtual void post_run();

  void setup_pre_force_respa(int, int);
  void initial_integrate_respa(int, int, int);
  void post_integrate_respa(int, int);
  void pre_force_respa(int, int, int);
  void post_force_respa(int, int, int);
  void final_integrate_respa(int, int);

  void setup_min_pre_force(int);
  void min_pre_exchange();
  void min_pre_force(int);
  void min_post_force(int);

  double min_energy(double *);
  void min_store();
  void min_step(double, double *);
  void min_clearstore();
  void min_pushstore();
  void min_popstore();
  int min_reset_ref();
  double max_alpha(double *);
  int min_dof();

  void add_fix(int, char **, char *suffix = NULL);
  void modify_fix(int, char **);
  void delete_fix(const char *);
  int find_fix(const char *);

  void add_compute(int, char **, char *suffix = NULL);
  void modify_compute(int, char **);
  void delete_compute(const char *);
  int find_compute(const char *);
  void clearstep_compute();
  void addstep_compute(bigint);
  void addstep_compute_all(bigint);

  void write_restart(FILE *);
  int read_restart(FILE *);
  void restart_deallocate();

  bigint memory_usage();

 protected:

  // lists of fixes to apply at different stages of timestep

  int *list_initial_integrate,*list_post_integrate;
  int *list_pre_exchange,*list_pre_neighbor;
  int *list_pre_force,*list_post_force;
  int *list_final_integrate,*list_end_of_step,*list_thermo_energy;
  int *list_initial_integrate_respa,*list_post_integrate_respa;
  int *list_pre_force_respa,*list_post_force_respa;
  int *list_final_integrate_respa;
  int *list_min_pre_exchange,*list_min_pre_force;
  int *list_min_post_force,*list_min_energy;

  int *end_of_step_every;

  int n_timeflag;            // list of computes that store time invocation
  int *list_timeflag;

  char **id_restart_global;           // stored fix global info
  char **style_restart_global;        // from read-in restart file
  char **state_restart_global;

  char **id_restart_peratom;          // stored fix peratom info
  char **style_restart_peratom;       // from read-in restart file
  int *index_restart_peratom;

  int index_permanent;        // fix/compute index returned to library call

  void list_init(int, int &, int *&);
  void list_init_end_of_step(int, int &, int *&);
  void list_init_thermo_energy(int, int &, int *&);
  void list_init_compute();
};

}

#endif

/* ERROR/WARNING messages:

W: One or more atoms are time integrated more than once

This is probably an error since you typically do not want to
advance the positions or velocities of an atom more than once
per timestep.

E: Fix command before simulation box is defined

The fix command cannot be used before a read_data, read_restart, or
create_box command.

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Could not find fix group ID

A group ID used in the fix command does not exist.

E: Replacing a fix, but new style != old style

A fix ID can be used a 2nd time, but only if the style matches the
previous fix.  In this case it is assumed you with to reset a fix's
parameters.  This error may mean you are mistakenly re-using a fix ID
when you do not intend to.

W: Replacing a fix, but new group != old group

The ID and style of a fix match for a fix you are changing with a fix
command, but the new group you are specifying does not match the old
group.

E: Invalid fix style

The choice of fix style is unknown.

E: Could not find fix_modify ID

A fix ID used in the fix_modify command does not exist.

E: Could not find fix ID to delete

Self-explanatory.

E: Reuse of compute ID

A compute ID cannot be used twice.

E: Invalid compute style

Self-explanatory.

E: Could not find compute_modify ID

Self-explanatory.

E: Could not find compute ID to delete

Self-explanatory.

*/

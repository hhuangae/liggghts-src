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

#include "lmptype.h"
#include "mpi.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "dump.h"
#include "atom.h"
#include "irregular.h"
#include "update.h"
#include "domain.h"
#include "group.h"
#include "output.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

// allocate space for static class variable

Dump *Dump::dumpptr;

#define BIG 1.0e20
#define IBIG 2147483647
#define EPSILON 1.0e-6

enum{ASCEND,DESCEND};

/* ---------------------------------------------------------------------- */

Dump::Dump(LAMMPS *lmp, int narg, char **arg) : Pointers(lmp)
{
  MPI_Comm_rank(world,&me);
  MPI_Comm_size(world,&nprocs);

  int n = strlen(arg[0]) + 1;
  id = new char[n];
  strcpy(id,arg[0]);

  igroup = group->find(arg[1]);
  groupbit = group->bitmask[igroup];

  n = strlen(arg[2]) + 1;
  style = new char[n];
  strcpy(style,arg[2]);

  n = strlen(arg[4]) + 1;
  filename = new char[n];
  strcpy(filename,arg[4]);

  comm_forward = comm_reverse = 0;

  first_flag = 0;
  flush_flag = 1;
  format = NULL;
  format_user = NULL;
  clearstep = 0;
  sort_flag = 0;
  append_flag = 0;
  padflag = 0;

  maxbuf = maxids = maxsort = maxproc = 0;
  buf = bufsort = NULL;
  ids = idsort = index = proclist = NULL;
  irregular = NULL;

  // parse filename for special syntax
  // if contains '%', write one file per proc and replace % with proc-ID
  // if contains '*', write one file per timestep and replace * with timestep
  // check file suffixes
  //   if ends in .bin = binary file
  //   else if ends in .gz = gzipped text file
  //   else ASCII text file

  fp = NULL;
  singlefile_opened = 0;
  compressed = 0;
  binary = 0;
  multifile = 0;
  multiproc = 0;

  char *ptr;
  if (ptr = strchr(filename,'%')) {
    multiproc = 1;
    char *extend = new char[strlen(filename) + 16];
    *ptr = '\0';
    sprintf(extend,"%s%d%s",filename,me,ptr+1);
    delete [] filename;
    n = strlen(extend) + 1;
    filename = new char[n];
    strcpy(filename,extend);
    delete [] extend;
  }

  if (strchr(filename,'*')) multifile = 1;

  char *suffix = filename + strlen(filename) - strlen(".bin");
  if (suffix > filename && strcmp(suffix,".bin") == 0) binary = 1;
  suffix = filename + strlen(filename) - strlen(".gz");
  if (suffix > filename && strcmp(suffix,".gz") == 0) compressed = 1;
}

/* ---------------------------------------------------------------------- */

Dump::~Dump()
{
  delete [] id;
  delete [] style;
  delete [] filename;

  delete [] format;
  delete [] format_default;
  delete [] format_user;

  memory->destroy(buf);
  memory->destroy(bufsort);
  memory->destroy(ids);
  memory->destroy(idsort);
  memory->destroy(index);
  memory->destroy(proclist);
  delete irregular;

  // XTC style sets fp to NULL since it closes file in its destructor

  if (multifile == 0 && fp != NULL) {
    if (compressed) {
      if (multiproc) pclose(fp);
      else if (me == 0) pclose(fp);
    } else {
      if (multiproc) fclose(fp);
      else if (me == 0) fclose(fp);
    }
  }
}

/* ---------------------------------------------------------------------- */

void Dump::init()
{
  init_style();

  if (!sort_flag) {
    memory->destroy(bufsort);
    memory->destroy(ids);
    memory->destroy(idsort);
    memory->destroy(index);
    memory->destroy(proclist);
    delete irregular;

    maxids = maxsort = maxproc = 0;
    bufsort = NULL;
    ids = idsort = index = proclist = NULL;
    irregular = NULL;
  }

  if (sort_flag) {
    if (sortcol == 0 && atom->tag_enable == 0)
      error->all(FLERR,"Cannot dump sort on atom IDs with no atom IDs defined");
    if (sortcol && sortcol > size_one)
      error->all(FLERR,"Dump sort column is invalid");
    if (nprocs > 1 && irregular == NULL)
      irregular = new Irregular(lmp);

    bigint size = group->count(igroup);
    if (size > MAXSMALLINT) error->all(FLERR,"Too many atoms to dump sort");

    // set reorderflag = 1 if can simply reorder local atoms rather than sort
    // criteria: sorting by ID, atom IDs are consecutive from 1 to Natoms
    //           min/max IDs of group match size of group
    // compute ntotal_reorder, nme_reorder, idlo/idhi to test against later

    reorderflag = 0;
    if (sortcol == 0 && atom->tag_consecutive()) {
      int *tag = atom->tag;
      int *mask = atom->mask;
      int nlocal = atom->nlocal;

      int min = IBIG;
      int max = 0;
      for (int i = 0; i < nlocal; i++)
	if (mask[i] & groupbit) {
	  min = MIN(min,tag[i]);
	  max = MAX(max,tag[i]);
	}
      int minall,maxall;
      MPI_Allreduce(&min,&minall,1,MPI_INT,MPI_MIN,world);
      MPI_Allreduce(&max,&maxall,1,MPI_INT,MPI_MAX,world);
      int isize = static_cast<int> (size);

      if (maxall-minall+1 == isize) {
	reorderflag = 1;
	double range = maxall-minall + EPSILON;
	idlo = static_cast<int> (range*me/nprocs + minall);
	int idhi = static_cast<int> (range*(me+1)/nprocs + minall);

	int lom1 = static_cast<int> ((idlo-1-minall)/range * nprocs);
	int lo = static_cast<int> ((idlo-minall)/range * nprocs);
	int him1 = static_cast<int> ((idhi-1-minall)/range * nprocs);
	int hi = static_cast<int> ((idhi-minall)/range * nprocs);
	if (me && me == lom1) idlo--;
	else if (me && me != lo) idlo++;
	if (me+1 == him1) idhi--;
	else if (me+1 != hi) idhi++;

	nme_reorder = idhi-idlo;
	ntotal_reorder = isize;
      }
    }
  }
}

/* ---------------------------------------------------------------------- */

void Dump::write()
{
  // if file per timestep, open new file

  if (multifile) openfile();

  // simulation box bounds

  if (domain->triclinic == 0) {
    boxxlo = domain->boxlo[0];
    boxxhi = domain->boxhi[0];
    boxylo = domain->boxlo[1];
    boxyhi = domain->boxhi[1];
    boxzlo = domain->boxlo[2];
    boxzhi = domain->boxhi[2];
  } else {
    boxxlo = domain->boxlo_bound[0];
    boxxhi = domain->boxhi_bound[0];
    boxylo = domain->boxlo_bound[1];
    boxyhi = domain->boxhi_bound[1];
    boxzlo = domain->boxlo_bound[2];
    boxzhi = domain->boxhi_bound[2];
    boxxy = domain->xy;
    boxxz = domain->xz;
    boxyz = domain->yz;
  }

  // nme = # of dump lines this proc will contribute to dump

  nme = count();
  bigint bnme = nme;

  // ntotal = total # of dump lines
  // nmax = max # of dump lines on any proc

  int nmax;
  if (multiproc) nmax = nme;
  else {
    MPI_Allreduce(&bnme,&ntotal,1,MPI_LMP_BIGINT,MPI_SUM,world);
    MPI_Allreduce(&nme,&nmax,1,MPI_INT,MPI_MAX,world);
  }

  // write timestep header

  if (multiproc) write_header(bnme);
  else write_header(ntotal);

  // insure proc 0 can receive everyone's info
  // limit nmax*size_one to int since used as arg in MPI_Rsend() below
  // pack my data into buf
  // if sorting on IDs also request ID list from pack()
  // sort buf as needed

  if (nmax > maxbuf) {
    if ((bigint) nmax * size_one > MAXSMALLINT)
      error->all(FLERR,"Too much per-proc info for dump");
    maxbuf = nmax;
    memory->destroy(buf);
    memory->create(buf,maxbuf*size_one,"dump:buf");
  }
  if (sort_flag && sortcol == 0 && nmax > maxids) {
    maxids = nmax;
    memory->destroy(ids);
    memory->create(ids,maxids,"dump:ids");
  }

  if (sort_flag && sortcol == 0) pack(ids);
  else pack(NULL);
  if (sort_flag) sort();

  // multiproc = 1 = each proc writes own data to own file 
  // multiproc = 0 = all procs write to one file thru proc 0
  //   proc 0 pings each proc, receives it's data, writes to file
  //   all other procs wait for ping, send their data to proc 0

  if (multiproc) write_data(nme,buf);
  else {
    int tmp,nlines;
    MPI_Status status;
    MPI_Request request;

    if (me == 0) {
      for (int iproc = 0; iproc < nprocs; iproc++) {
	if (iproc) {
	  MPI_Irecv(buf,maxbuf*size_one,MPI_DOUBLE,iproc,0,world,&request);
	  MPI_Send(&tmp,0,MPI_INT,iproc,0,world);
	  MPI_Wait(&request,&status);
	  MPI_Get_count(&status,MPI_DOUBLE,&nlines);
	  nlines /= size_one;
	} else nlines = nme;

	write_data(nlines,buf);
      }
      if (flush_flag) fflush(fp);
      
    } else {
      MPI_Recv(&tmp,0,MPI_INT,0,0,world,&status);
      MPI_Rsend(buf,nme*size_one,MPI_DOUBLE,0,0,world);
    }
  }

  // if file per timestep, close file

  if (multifile) {
    if (compressed) {
      if (multiproc) pclose(fp);
      else if (me == 0) pclose(fp);
    } else {
      if (multiproc) fclose(fp);
      else if (me == 0) fclose(fp);
    }
  }
}

/* ----------------------------------------------------------------------
   generic opening of a dump file
   ASCII or binary or gzipped
   some derived classes override this function
------------------------------------------------------------------------- */

void Dump::openfile()
{
  // single file, already opened, so just return

  if (singlefile_opened) return;
  if (multifile == 0) singlefile_opened = 1;

  // if one file per timestep, replace '*' with current timestep

  char *filecurrent;
  if (multifile == 0) filecurrent = filename;
  else {
    filecurrent = new char[strlen(filename) + 16];
    char *ptr = strchr(filename,'*');
    *ptr = '\0';
    if (padflag == 0) 
      sprintf(filecurrent,"%s" BIGINT_FORMAT "%s",
	      filename,update->ntimestep,ptr+1);
    else {
      char bif[8],pad[16];
      strcpy(bif,BIGINT_FORMAT);
      sprintf(pad,"%%s%%0%d%s%%s",padflag,&bif[1]);
      sprintf(filecurrent,pad,filename,update->ntimestep,ptr+1);
    }
    *ptr = '*';
  }

  // open one file on proc 0 or file on every proc

  if (me == 0 || multiproc) {
    if (compressed) {
#ifdef LAMMPS_GZIP
      char gzip[128];
      sprintf(gzip,"gzip -6 > %s",filecurrent);
      fp = popen(gzip,"w");
#else
      error->one(FLERR,"Cannot open gzipped file");
#endif
    } else if (binary) {
      fp = fopen(filecurrent,"wb");
    } else if (append_flag) {
      fp = fopen(filecurrent,"a");
    } else {
      fp = fopen(filecurrent,"w");
    }

    if (fp == NULL) error->one(FLERR,"Cannot open dump file");
  } else fp = NULL;

  // delete string with timestep replaced

  if (multifile) delete [] filecurrent;
}

/* ----------------------------------------------------------------------
   parallel sort of buf across all procs
   changes nme, reorders datums in buf, grows buf if necessary
------------------------------------------------------------------------- */

void Dump::sort()
{
  int i,iproc;
  double value;

  // if single proc, swap ptrs to buf,ids <-> bufsort,idsort

  if (nprocs == 1) {
    if (nme > maxsort) {
      maxsort = nme;
      memory->destroy(bufsort);
      memory->create(bufsort,maxsort*size_one,"dump:bufsort");
      memory->destroy(index);
      memory->create(index,maxsort,"dump:index");
      if (sortcol == 0) {
	memory->destroy(idsort);
	memory->create(idsort,maxsort,"dump:idsort");
      }
    }

    double *dptr = buf;
    buf = bufsort;
    bufsort = dptr;

    if (sortcol == 0) {
      int *iptr = ids;
      ids = idsort;
      idsort = iptr;
    }

  // if multiple procs, exchange datums between procs via irregular
    
  } else {

    // grow proclist if necessary

    if (nme > maxproc) {
      maxproc = nme;
      memory->destroy(proclist);
      memory->create(proclist,maxproc,"dump:proclist");
    }
    
    // proclist[i] = which proc Ith datum will be sent to

    if (sortcol == 0) {
      int min = IBIG;
      int max = 0;
      for (i = 0; i < nme; i++) {
	min = MIN(min,ids[i]);
	max = MAX(max,ids[i]);
      }
      int minall,maxall;
      MPI_Allreduce(&min,&minall,1,MPI_INT,MPI_MIN,world);
      MPI_Allreduce(&max,&maxall,1,MPI_INT,MPI_MAX,world);
      double range = maxall-minall + EPSILON;
      for (i = 0; i < nme; i++) {
	iproc = static_cast<int> ((ids[i]-minall)/range * nprocs);
	proclist[i] = iproc;
      }

    } else {
      double min = BIG;
      double max = -BIG;
      for (i = 0; i < nme; i++) {
	value = buf[i*size_one + sortcolm1];
	min = MIN(min,value);
	max = MAX(max,value);
      }
      double minall,maxall;
      MPI_Allreduce(&min,&minall,1,MPI_DOUBLE,MPI_MIN,world);
      MPI_Allreduce(&max,&maxall,1,MPI_DOUBLE,MPI_MAX,world);
      double range = maxall-minall + EPSILON*(maxall-minall);
      if (range == 0.0) range = EPSILON;
      for (i = 0; i < nme; i++) {
	value = buf[i*size_one + sortcolm1];
	iproc = static_cast<int> ((value-minall)/range * nprocs);
	proclist[i] = iproc;
      }
    }

    // create comm plan, grow recv bufs if necessary,
    // exchange datums, destroy plan
    // if sorting on atom IDs, exchange IDs also

    nme = irregular->create_data(nme,proclist);

    if (nme > maxsort) {
      maxsort = nme;
      memory->destroy(bufsort);
      memory->create(bufsort,maxsort*size_one,"dump:bufsort");
      memory->destroy(index);
      memory->create(index,maxsort,"dump:index");
      if (sortcol == 0) {
	memory->destroy(idsort);
	memory->create(idsort,maxsort,"dump:idsort");
      }
    }
    
    irregular->exchange_data((char *) buf,size_one*sizeof(double),
			     (char *) bufsort);
    if (sortcol == 0) 
      irregular->exchange_data((char *) ids,sizeof(int),(char *) idsort);
    irregular->destroy_data();
  }

  // if reorder flag is set & total/per-proc counts match pre-computed values,
  // then create index directly from idsort
  // else quicksort of index using IDs or buf column as comparator

  if (reorderflag) {
    if (ntotal != ntotal_reorder) reorderflag = 0;
    int flag = 0;
    if (nme != nme_reorder) flag = 1;
    int flagall;
    MPI_Allreduce(&flag,&flagall,1,MPI_INT,MPI_SUM,world);
    if (flagall) reorderflag = 0;

    if (reorderflag)
      for (i = 0; i < nme; i++)
	index[idsort[i]-idlo] = i;
  }

  if (!reorderflag) {
    dumpptr = this;
    for (i = 0; i < nme; i++) index[i] = i;
    if (sortcol == 0) qsort(index,nme,sizeof(int),idcompare);
    else if (sortorder == ASCEND) qsort(index,nme,sizeof(int),bufcompare);
    else qsort(index,nme,sizeof(int),bufcompare_reverse);
  }

  // reset buf size and maxbuf to largest of any post-sort nme values
  // this insures proc 0 can receive everyone's info

  int nmax;
  if (multiproc) nmax = nme;
  else MPI_Allreduce(&nme,&nmax,1,MPI_INT,MPI_MAX,world);

  if (nmax > maxbuf) {
    maxbuf = nmax;
    memory->destroy(buf);
    memory->create(buf,maxbuf*size_one,"dump:buf");
  }

  // copy data from bufsort to buf using index
    
  int nbytes = size_one*sizeof(double);
  for (i = 0; i < nme; i++)
    memcpy(&buf[i*size_one],&bufsort[index[i]*size_one],nbytes);
}

/* ----------------------------------------------------------------------
   compare two atom IDs
   called via qsort() in sort() method
   is a static method so access data via dumpptr
------------------------------------------------------------------------- */

int Dump::idcompare(const void *pi, const void *pj)
{
  int *idsort = dumpptr->idsort;

  int i = *((int *) pi);
  int j = *((int *) pj);

  if (idsort[i] < idsort[j]) return -1;
  if (idsort[i] > idsort[j]) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   compare two buffer values with size_one stride
   called via qsort() in sort() method
   is a static method so access data via dumpptr
   sort in ASCENDing order
------------------------------------------------------------------------- */

int Dump::bufcompare(const void *pi, const void *pj)
{
  double *bufsort = dumpptr->bufsort;
  int size_one = dumpptr->size_one;
  int sortcolm1 = dumpptr->sortcolm1;

  int i = *((int *) pi)*size_one + sortcolm1;
  int j = *((int *) pj)*size_one + sortcolm1;

  if (bufsort[i] < bufsort[j]) return -1;
  if (bufsort[i] > bufsort[j]) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   compare two buffer values with size_one stride
   called via qsort() in sort() method
   is a static method so access data via dumpptr
   sort in DESCENDing order
------------------------------------------------------------------------- */

int Dump::bufcompare_reverse(const void *pi, const void *pj)
{
  double *bufsort = dumpptr->bufsort;
  int size_one = dumpptr->size_one;
  int sortcolm1 = dumpptr->sortcolm1;

  int i = *((int *) pi)*size_one + sortcolm1;
  int j = *((int *) pj)*size_one + sortcolm1;

  if (bufsort[i] > bufsort[j]) return -1;
  if (bufsort[i] < bufsort[j]) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   process params common to all dumps here
   if unknown param, call modify_param specific to the dump
------------------------------------------------------------------------- */

void Dump::modify_params(int narg, char **arg)
{
  if (narg == 0) error->all(FLERR,"Illegal dump_modify command");

  int iarg = 0;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"append") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) append_flag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) append_flag = 0;
      else error->all(FLERR,"Illegal dump_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"every") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      int idump;
      for (idump = 0; idump < output->ndump; idump++)
	if (strcmp(id,output->dump[idump]->id) == 0) break;
      int n;
      if (strstr(arg[iarg+1],"v_") == arg[iarg+1]) {
	delete [] output->var_dump[idump];
	n = strlen(&arg[iarg+1][2]) + 1;
	output->var_dump[idump] = new char[n];
	strcpy(output->var_dump[idump],&arg[iarg+1][2]);
	n = 0;
      } else {
	n = atoi(arg[iarg+1]);
	if (n <= 0) error->all(FLERR,"Illegal dump_modify command");
      }
      output->every_dump[idump] = n;
      iarg += 2;
    } else if (strcmp(arg[iarg],"first") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) first_flag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) first_flag = 0;
      else error->all(FLERR,"Illegal dump_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"flush") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      if (strcmp(arg[iarg+1],"yes") == 0) flush_flag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) flush_flag = 0;
      else error->all(FLERR,"Illegal dump_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"format") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      delete [] format_user;
      format_user = NULL;
      if (strcmp(arg[iarg+1],"none")) {
	int n = strlen(arg[iarg+1]) + 1;
	format_user = new char[n];
	strcpy(format_user,arg[iarg+1]);
      }
      iarg += 2;
    } else if (strcmp(arg[iarg],"pad") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      padflag = atoi(arg[iarg+1]);
      if (padflag < 0) error->all(FLERR,"Illegal dump_modify command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"sort") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal dump_modify command");
      if (strcmp(arg[iarg+1],"off") == 0) sort_flag = 0;
      else if (strcmp(arg[iarg+1],"id") == 0) {
	sort_flag = 1;
	sortcol = 0;
	sortorder = ASCEND;
      } else {
	sort_flag = 1;
	sortcol = atoi(arg[iarg+1]);
	sortorder = ASCEND;
	if (sortcol == 0) error->all(FLERR,"Illegal dump_modify command");
	if (sortcol < 0) {
	  sortorder = DESCEND;
	  sortcol = -sortcol;
	}
	sortcolm1 = sortcol - 1;
      }
      iarg += 2;
    } else {
      int n = modify_param(narg-iarg,&arg[iarg]);
      if (n == 0) error->all(FLERR,"Illegal dump_modify command");
      iarg += n;
    }
  }
}

/* ----------------------------------------------------------------------
   return # of bytes of allocated memory
------------------------------------------------------------------------- */

bigint Dump::memory_usage()
{
  bigint bytes = memory->usage(buf,size_one*maxbuf);
  if (sort_flag) {
    if (sortcol == 0) bytes += memory->usage(ids,maxids);
    bytes += memory->usage(bufsort,size_one*maxsort);
    if (sortcol == 0) bytes += memory->usage(idsort,maxsort);
    bytes += memory->usage(index,maxsort);
    bytes += memory->usage(proclist,maxproc);
    if (irregular) bytes += irregular->memory_usage();
  }
  return bytes;
}

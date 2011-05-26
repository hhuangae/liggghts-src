# LAMMPS multiple-machine Makefile

SHELL = /bin/sh
#.IGNORE:

# Definitions

ROOT =	lmp
EXE =	$(ROOT)_$@
SRC =	$(wildcard *.cpp)
INC =	$(wildcard *.h)
OBJ = 	$(SRC:.cpp=.o)

# Package variables

PACKAGE = asphere class2 colloid dipole dsmc gpu granular \
	  kspace manybody meam molecule opt peri poems reax replica \
	  shock srd xtc

PACKUSER = user-ackland user-atc user-cd-eam user-cg-cmm user-cuda \
	   user-eff user-ewaldn user-imd user-reaxc user-smd

PACKALL = $(PACKAGE) $(PACKUSER)

PACKAGEUC = $(shell echo $(PACKAGE) | tr a-z A-Z)
PACKUSERUC = $(shell echo $(PACKUSER) | tr a-z A-Z)

YESDIR = $(shell echo $(@:yes-%=%) | tr a-z A-Z)
NODIR  = $(shell echo $(@:no-%=%) | tr a-z A-Z)

# List of all targets

help:
	@echo ''
	@echo 'make clean-all           delete all object files'
	@echo 'make clean-machine       delete object files for one machine'
	@echo 'make tar                 lmp_src.tar.gz of src dir and packages'
	@echo 'make makelib             update Makefile.lib for library build'
	@echo 'make makelist            update Makefile.list used by old makes'
	@echo ''
	@echo 'make package             list available packages'
	@echo 'make package-status      status of all packages'
	@echo 'make yes-package         install a single package in src dir'
	@echo 'make no-package          remove a single package from src dir'
	@echo 'make yes-all             install all packages in src dir'
	@echo 'make no-all              remove all packages from src dir'
	@echo 'make yes-standard        install all standard packages'
	@echo 'make no-standard         remove all standard packages'
	@echo 'make yes-user            install all user packages'
	@echo 'make no-user             remove all user packages'
	@echo ''
	@echo 'make package-update      replace src files with package files'
	@echo 'make package-overwrite   replace package files with src files'
	@echo ''
	@echo 'make machine             build LAMMPS where machine is one of:'
	@echo ''
	@files="`ls MAKE/Makefile.*`"; \
	  for file in $$files; do head -1 $$file; done
	@echo ''

# Build the code

.DEFAULT:
	@test -f MAKE/Makefile.$@
	@if [ ! -d Obj_$@ ]; then mkdir Obj_$@; fi
	@$(SHELL) Make.sh style
	@cp -p *.cpp *.h Obj_$@
	@cp MAKE/Makefile.$@ Obj_$@/Makefile
	@if [ ! -e Makefile.package ]; then make package-regenerate; fi
	@cp Makefile.package Obj_$@
	@cd Obj_$@; \
	$(MAKE) $(MFLAGS) "OBJ = $(OBJ)" "INC = $(INC)" "EXE = ../$(EXE)" ../$(EXE)
	@if [ -d Obj_$@ ]; then cd Obj_$@; rm -f $(SRC) $(INC) Makefile*; fi

# Remove machine-specific object files

clean:
	@echo 'make clean-all           delete all object files'
	@echo 'make clean-machine       delete object files for one machine'

clean-all:
	rm -rf Obj_*

clean-%:
	rm -rf Obj_$(@:clean-%=%)

# Create a tarball of src dir and packages

tar:
	@cd STUBS; make clean
	@cd ..; tar cvzf src/$(ROOT)_src.tar.gz \
	  src/Make* src/Package.sh src/MAKE src/*.cpp src/*.h src/STUBS \
	  $(patsubst %,src/%,$(PACKAGEUC)) $(patsubst %,src/%,$(PACKUSERUC)) \
          --exclude=*/.svn
	@cd STUBS; make
	@echo "Created $(ROOT)_src.tar.gz"

# Update Makefile.lib and Makefile.list

makelib:
	@$(SHELL) Make.sh style
	@$(SHELL) Make.sh Makefile.lib

makelist:
	@$(SHELL) Make.sh style
	@$(SHELL) Make.sh Makefile.list

# Package management

package:
	@echo 'Standard packages:' $(PACKAGE)
	@echo ''
	@echo 'User-contributed packages:' $(PACKUSER)
	@echo ''
	@echo 'make package             list available packages'
	@echo 'make package-status      status of all packages'
	@echo 'make yes-package         install a single package in src dir'
	@echo 'make no-package          remove a single package from src dir'
	@echo 'make yes-all             install all packages in src dir'
	@echo 'make no-all              remove all packages from src dir'
	@echo 'make yes-standard        install all standard packages'
	@echo 'make no-standard         remove all standard packages'
	@echo 'make yes-user            install all user packages'
	@echo 'make no-user             remove all user packages'
	@echo ''
	@echo 'make package-update      replace src files with package files'
	@echo 'make package-overwrite   replace package files with src files'

yes-all:
	@for p in $(PACKALL); do $(MAKE) yes-$$p; done

no-all:
	@for p in $(PACKALL); do $(MAKE) no-$$p; done

yes-standard:
	@for p in $(PACKAGE); do $(MAKE) yes-$$p; done

no-standard:
	@for p in $(PACKAGE); do $(MAKE) no-$$p; done

yes-user:
	@for p in $(PACKUSER); do $(MAKE) yes-$$p; done

no-user:
	@for p in $(PACKUSER); do $(MAKE) no-$$p; done

yes-%:
	@if [ ! -e $(YESDIR) ]; then \
	  echo "Package $(@:yes-%=%) does not exist"; \
	else \
	  echo "Installing package $(@:yes-%=%)"; \
	  cd $(YESDIR); $(SHELL) Install.sh 1; cd ..; $(SHELL) Depend.sh 1; \
	fi;

no-%:
	@if [ ! -e $(NODIR) ]; then \
	  echo "Package $(@:no-%=%) does not exist"; \
	else \
	  echo "Uninstalling package $(@:no-%=%), ignore errors"; \
	  cd $(NODIR); $(SHELL) Install.sh 0; cd ..; $(SHELL) Depend.sh 0; \
        fi;

# status = list differences between src and package files
# update = replace src files with newer package files
# overwrite = overwrite package files with newer src files
# regenerate = regenerate Makefile.package from Makefile.package.empty

package-status:
	@for p in $(PACKAGEUC); do $(SHELL) Package.sh $$p status; done
	@echo ''
	@for p in $(PACKUSERUC); do $(SHELL) Package.sh $$p status; done

package-update:
	@for p in $(PACKAGEUC); do $(SHELL) Package.sh $$p update; done
	@echo ''
	@for p in $(PACKUSERUC); do $(SHELL) Package.sh $$p update; done

package-overwrite:
	@for p in $(PACKAGEUC); do $(SHELL) Package.sh $$p overwrite; done
	@echo ''
	@for p in $(PACKUSERUC); do $(SHELL) Package.sh $$p overwrite; done

package-regenerate:
	@cp Makefile.package.empty Makefile.package
	@for p in $(PACKAGEUC); do $(SHELL) Package.sh $$p regenerate; done
	@for p in $(PACKUSERUC); do $(SHELL) Package.sh $$p regenerate; done

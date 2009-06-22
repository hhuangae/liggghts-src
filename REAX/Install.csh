# Install/unInstall package classes in LAMMPS

if ($1 == 1) then

  cp style_reax.h ..

  cp pair_reax.cpp ..

  cp pair_reax.h ..
  cp pair_reax_fortran.h ..

  cp fix_reax_bonds.h ..
  cp fix_reax_bonds.cpp ..

else if ($1 == 0) then

  rm ../style_reax.h
  touch ../style_reax.h

  rm ../pair_reax.cpp

  rm ../pair_reax.h
  rm ../pair_reax_fortran.h

  rm ../fix_reax_bonds.h
  rm ../fix_reax_bonds.cpp

endif

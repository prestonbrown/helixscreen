#####################################################################
# C compiler

#CC= clang
#CC=cc
CC=gcc
#CC=tcc
#uncomment the following line for compiling the lib using tinyc.
#note that the demos will fail to build with tinyc, but you can
#-link- to tinygl built with tinyc using gcc.
#CC= tcc
#CFLAGS= -Wall -w -O3 -g -std=c99 -mtune=native -DNDEBUG
#CFLAGS= -Wall -w -O3 -g -std=c99 -march=native -DNDEBUG

#####################################################################
# Multi-threading support (OpenMP)
# Set to 1 to enable parallel pixel/texture operations
# Requires OpenMP support in compiler (gcc -fopenmp)
# Default: 0 (single-threaded, simpler builds for embedded)
TGL_ENABLE_OPENMP ?= 0

ifeq ($(TGL_ENABLE_OPENMP),1)
  OPENMP_FLAGS = -fopenmp
else
  OPENMP_FLAGS =
endif

#####################################################################
#extract the maximum possible performance from gcc.

#CFLAGS= -Wall -O3 -std=c99 -DNDEBUG -march=native -Wno-unused-function
CFLAGS= -Wall -O3 -std=c99 -DNDEBUG -march=native -Wno-unused-function $(OPENMP_FLAGS)
#CFLAGS_LIB= -Wall -O3 -std=c99 -pedantic -DNDEBUG -march=native -Wno-unused-function
CFLAGS_LIB= -Wall -O3 -std=c99 -pedantic -DNDEBUG -march=native -Wno-unused-function $(OPENMP_FLAGS)


#CFLAGS= -Wall -Ofast -std=gnu99 -DNDEBUG -mtune=native
#uncomment the following line for compiling the lib with tinyc
#keep these flags for compiling with gcc if you are linking with tinygl compiled with tcc.
#CFLAGS= -Wall -O3 -std=gnu99 -DNDEBUG -g
#CFLAGS= -Wall -O1 -g -std=c99 -Wno-undef -DNDEBUG
LFLAGS=$(OPENMP_FLAGS)





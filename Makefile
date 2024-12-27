DPU_DIR := dpu
HOST_DIR := host
BUILDDIR ?= bin
NR_TASKLETS ?= 12
NR_DPUS ?= 512

COMMON_INCLUDES := support
HOST_TARGET := ${BUILDDIR}/pimlex_host
DPU_TARGET := ${BUILDDIR}/pimlex_dpu


HOST_SOURCES := $(wildcard ${HOST_DIR}/pimlex_host.cpp)
DPU_SOURCES := $(wildcard ${DPU_DIR}/task.c)

.PHONY: all clean

__dirs := $(shell mkdir -p ${BUILDDIR})

COMMON_FLAGS := -g -I${COMMON_INCLUDES}
HOST_FLAGS := ${COMMON_FLAGS} -std=c++17 -fopenmp -ltbb -ljemalloc -O3 -march=native `dpu-pkg-config --cflags --libs dpu` -DNR_TASKLETS=${NR_TASKLETS} -DNR_DPUS=${NR_DPUS}
DPU_FLAGS := ${COMMON_FLAGS} -O3 -DNR_TASKLETS=${NR_TASKLETS}

all: ${HOST_TARGET} ${DPU_TARGET}


${HOST_TARGET}: ${HOST_SOURCES} ${COMMON_INCLUDES}
	g++ -o $@ ${HOST_SOURCES} ${HOST_FLAGS}

${DPU_TARGET}: ${DPU_SOURCES} ${COMMON_INCLUDES}
	dpu-upmem-dpurte-clang ${DPU_FLAGS} -o $@ ${DPU_SOURCES}

clean:
	$(RM) -r $(BUILDDIR)

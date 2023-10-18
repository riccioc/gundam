FROM rootproject/root as base

SHELL ["/bin/bash", "-c"]

RUN apt-get dist-upgrade -y 
RUN apt-get update && apt-get upgrade -y
RUN apt-get install git libyaml-cpp-dev nlohmann-json3-dev -y

ENV WORK_DIR /home/work
ENV REPO_DIR $WORK_DIR/repo
ENV BUILD_DIR $WORK_DIR/build
ENV INSTALL_DIR $WORK_DIR/install

RUN mkdir -p $REPO_DIR
RUN mkdir -p $BUILD_DIR
RUN mkdir -p $INSTALL_DIR


# Copying GUNDAM source files
COPY ./src $REPO_DIR/src
COPY ./submodules $REPO_DIR/submodules
COPY ./cmake $REPO_DIR/cmake
COPY ./CMakeLists.txt $REPO_DIR/CMakeLists.txt
COPY ./tests $REPO_DIR/tests
#COPY ./.git $REPO_DIR/.git


# Checking out missing code
WORKDIR $REPO_DIR
RUN #git submodule update --init --recursive


# Now build GUNDAM
WORKDIR $BUILD_DIR
RUN cmake \
      -D CMAKE_INSTALL_PREFIX=$INSTALL_DIR \
      -D ENABLE_CUDA=ON \
      $REPO_DIR
RUN make -j3 install

# setup env
RUN . $INSTALL_DIR/setup.sh

# run the tests
RUN CTEST_OUTPUT_ON_FAILURE=1 make test

# End of the file

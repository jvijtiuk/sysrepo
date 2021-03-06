#install dependencies using homebrew
brew update
brew install pcre
brew install swig@4

set -e

INSTALL_PREFIX_DIR=$HOME/local
export PKG_CONFIG_PATH=$INSTALL_PREFIX_DIR/lib/pkgconfig:$PKG_CONFIG_PATH

# check to see if cache folder is empty
if [ ! -d "$INSTALL_PREFIX_DIR/lib" ]; then
    echo "Building all libraries."
    cd ~

    # CMocka
    git clone git://git.cryptomilk.org/projects/cmocka.git
    cd cmocka ; mkdir build; cd build
    cmake ..
    make -j2 && make install
    cd ../..
else
    echo "Using cached libraries from $INSTALL_PREFIX_DIR"
fi

if [[ (( "$TRAVIS_BRANCH" == *"master"* )) || (( "$TRAVIS_TAG" =~ "v"[0-9]+"."[0-9]+"."[0-9]+ )) ]]; then
    git clone https://github.com/CESNET/libyang.git
else
    git clone -b devel https://github.com/CESNET/libyang.git
fi
cd libyang ; mkdir build ; cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DGEN_LANGUAGE_BINDINGS=ON -DENABLE_BUILD_TESTS=OFF ..
make -j2 && sudo make install
cd ../..

emcc --bind -s DEMANGLE_SUPPORT=1 -I ../../vcglib/ Color.cpp Measure.cpp Decorator.cpp CppMesh.cpp Meshing.cpp Sampling.cpp Create.cpp Refine.cpp Smooth.cpp Saver.cpp Selection.cpp ../../vcglib/wrap/ply/plylib.cpp  -s TOTAL_MEMORY=536870912 -O3 -o ../js/generated/MeshLabCppCore.js
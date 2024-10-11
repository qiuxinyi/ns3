#  build_example(
#   NAME credence-evaluation
#   SOURCE_FILES 
#     credence-evaluation.cc
#     cdf.c
#   HEADER_FILES
#     cdf.h
#  LIBRARIES_TO_LINK
#    ${libpoint-to-point}
#    ${libtraffic-control}
#    ${libinternet}
#    ${libapplications}
#    ${libpybind11}#要下载pybind11，然后进行安装，cd pybind11  cmake -S . -B build cmake --build build -j 8   sudo cmake --install build

# )
# build_example(
#   NAME credence-evaluation_muti
#   SOURCE_FILES 
#     credence-evaluation_muti.cc
#     cdf.c
#   HEADER_FILES
#     cdf.h
#  LIBRARIES_TO_LINK
#    ${libpoint-to-point}
#    ${libtraffic-control}
#    ${libinternet}
#    ${libapplications}
#    #${libpybind11}
# )
# set_source_files_properties(cdf.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

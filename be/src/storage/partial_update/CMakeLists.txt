# where to put generated libraries
set(LIBRARY_OUTPUT_PATH "${BUILD_DIR}/src/storage/partial_update")

# where to put generated binaries
set(EXECUTABLE_OUTPUT_PATH "${BUILD_DIR}/src/storage/partial_update")

ADD_BE_LIB(PartialUpdate
    partial_update_helper.cpp
    )

# PartialUpdate hosts the SDCG (Sparse Delta Column Group) write/overlay helpers.
# It sits near the top of the storage stack: it depends on Rowset (segment/Rowset
# types) and, transitively through Rowset and the executable link group, on the
# lower core layers (types/, column/, common/, base/, gutil/, gen_cpp/, fs/,
# serde/, runtime/, util/). Storage links PartialUpdate, not the other way round,
# so the partial-update sources can be lifted out of the Storage aggregate.
target_link_libraries(PartialUpdate PUBLIC Rowset)

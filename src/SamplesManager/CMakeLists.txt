# Samples
set( LIB_NAME GundamSamplesManager )

set( SRCFILES
    src/EventUtils.cpp
    src/Event.cpp
    src/SampleElement.cpp
    src/Sample.cpp
    src/SampleSet.cpp
    src/PlotGenerator.cpp
)

if( USE_STATIC_LINKS )
  add_library(${LIB_NAME} STATIC ${SRCFILES})
else()
  add_library(${LIB_NAME} SHARED ${SRCFILES})
endif()

target_include_directories(
    ${LIB_NAME}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    ${LIB_NAME}
    PUBLIC GundamParametersManager
)

#set_target_properties( ${LIB_NAME} PROPERTIES VERSION "${GUNDAM_VERSION_STRING}")

install(TARGETS ${LIB_NAME} DESTINATION lib)


cmake_minimum_required(VERSION 3.15)
project(jucePluginLib VERSION ${CMAKE_PROJECT_VERSION}) 

configure_file(${CMAKE_CURRENT_LIST_DIR}/version.h.in ${CMAKE_CURRENT_LIST_DIR}/version.h)

set(TUS_VERSION_DATETIME_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/jucePluginLib/versionDateTime.h")
# Configuration metadata stays in this build tree. configure_file changes the
# output only when its content changes (cmake.org/cmake/help/latest/command/configure_file.html).
include(${CMAKE_CURRENT_LIST_DIR}/createVersionDateTime.cmake)

set(SOURCES
	bypassBuffer.cpp bypassBuffer.h
	createVersionDateTime.cmake
	clipboard.cpp clipboard.h
	controller.cpp controller.h
	controllermap.cpp controllermap.h
	dummydevice.cpp dummydevice.h
	filetype.cpp filetype.h
	midiLearnManager.cpp midiLearnManager.h
	midiLearnMapping.cpp midiLearnMapping.h
	midiLearnPreset.cpp midiLearnPreset.h
	midiLearnTranslator.cpp midiLearnTranslator.h
	midiNotifier.cpp midiNotifier.h
	midipacket.cpp midipacket.h
	midiports.cpp midiports.h
	parameter.cpp parameter.h
	parameterdescription.cpp parameterdescription.h
	parameterdescriptions.cpp parameterdescriptions.h
	parameterlink.cpp parameterlink.h
	parameterlinks.cpp parameterlinks.h
	parameterlistener.cpp parameterlistener.h
	parameterlocking.cpp parameterlocking.h
	parameterregion.cpp parameterregion.h
	parametervaluelist.cpp parametervaluelist.h
	pluginVersion.cpp pluginVersion.h
	processor.cpp processor.h
	processorPropertiesInit.h
	programChangeRouter.cpp programChangeRouter.h
	skinVariables.cpp skinVariables.h
	softknob.cpp softknob.h
	tools.cpp tools.h
	types.h
	version.h.in version.h
	versionDateTime.h.in ${TUS_VERSION_DATETIME_HEADER}
)

set(SOURCES_PATCHDB
	patchdb/datasource.cpp patchdb/datasource.h
	patchdb/db.cpp patchdb/db.h
	patchdb/jobqueue.cpp patchdb/jobqueue.h
	patchdb/patch.cpp patchdb/patch.h
	patchdb/patchdbtypes.cpp patchdb/patchdbtypes.h
	patchdb/patchhistory.cpp patchdb/patchhistory.h
	patchdb/patchmodifications.cpp patchdb/patchmodifications.h
	patchdb/search.cpp patchdb/search.h
	patchdb/serialization.cpp patchdb/serialization.h
	patchdb/tags.cpp patchdb/tags.h
)

add_library(jucePluginLib STATIC)

target_sources(jucePluginLib PRIVATE ${SOURCES} ${SOURCES_PATCHDB})
source_group("source" FILES ${SOURCES})
source_group("source\\patchdb" FILES ${SOURCES_PATCHDB})

target_link_libraries(jucePluginLib PUBLIC juceUiLib synthLib bridgeClient)
target_compile_definitions(jucePluginLib PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1)
set_property(TARGET jucePluginLib PROPERTY FOLDER "Gearmulator")

target_include_directories(jucePluginLib BEFORE PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(jucePluginLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(UNIX AND NOT APPLE)
	set(THREADS_PREFER_PTHREAD_FLAG TRUE)
	find_package(Threads REQUIRED)
	target_link_libraries(jucePluginLib PRIVATE Threads::Threads)
endif()

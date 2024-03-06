function(NBL_GEN_FIND_NABLA_CODE_IMPL _COMPOMENT_ _SPATH_)
string(APPEND NBL_FIND_NABLA_IMPL "set(_COMPOMENT_ ${_COMPOMENT_})\nset(_SPATH_ ${_SPATH_})\n\nset(NBL_ROOT_PATH ${NBL_ROOT_PATH})\n\n")
string(APPEND NBL_FIND_NABLA_IMPL
[=[
if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    set(NBL_CONFIG_PREFIX_PATH debug)
elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    set(NBL_CONFIG_PREFIX_PATH relwithdebinfo)
elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
	unset(NBL_CONFIG_PREFIX_PATH)
else()
	message(FATAL_ERROR "Internal error, requested \"${CMAKE_INSTALL_CONFIG_NAME}\" configuration is invalid!")
endif()

string(REPLACE "${CMAKE_INSTALL_PREFIX}" "" NBL_CMAKE_INSTALL_MANIFEST_CONTENT "${CMAKE_INSTALL_MANIFEST_FILES}")
list(REMOVE_DUPLICATES NBL_CMAKE_INSTALL_MANIFEST_CONTENT)

set(_NBL_PREFIX_ "${CMAKE_INSTALL_PREFIX}/${NBL_CONFIG_PREFIX_PATH}")
string(TOUPPER "${CMAKE_INSTALL_CONFIG_NAME}" _NBL_CONFIG_)
string(TOUPPER "${_COMPOMENT_}" _Cu_)
string(TOLOWER "${_COMPOMENT_}" _Cl_)

set(NBL_CMAKE_COMPOMENT_OUTPUT_DIRECTORY "${CMAKE_INSTALL_PREFIX}/${NBL_CONFIG_PREFIX_PATH}/cmake/compoment")
set(NBL_CMAKE_COMPOMENT_OUTPUT_FILE "${NBL_CMAKE_COMPOMENT_OUTPUT_DIRECTORY}/NablaConfig${_COMPOMENT_}.cmake")

cmake_path(RELATIVE_PATH CMAKE_INSTALL_PREFIX BASE_DIRECTORY "${NBL_CMAKE_COMPOMENT_OUTPUT_DIRECTORY}" OUTPUT_VARIABLE _NBL_REL_TO_PREFIX_)

foreach(_MANIFEST_INSTALL_REL_FILE_ IN LISTS NBL_CMAKE_INSTALL_MANIFEST_CONTENT)
	string(FIND "${_MANIFEST_INSTALL_REL_FILE_}" "/${_SPATH_}/" _NBL_FOUND_)
	
	if(NOT "${_NBL_FOUND_}" STREQUAL "-1")
		set(_X_ "${_NBL_REL_TO_PREFIX_}/${_MANIFEST_INSTALL_REL_FILE_}")
		cmake_path(NORMAL_PATH _X_ OUTPUT_VARIABLE _X_)
		
		list(APPEND NBL_INSTALL_${_Cu_}_${_NBL_CONFIG_} "${_X_}")
	endif()
endforeach()

set(_NBL_PROXY_ NBL_INSTALL_${_Cu_}_${_NBL_CONFIG_})

string(APPEND NBL_MANIFEST_IMPL "set(${_NBL_PROXY_}\n\t${${_NBL_PROXY_}}\n)")
string(REPLACE ";" "\n\t" NBL_MANIFEST_IMPL "${NBL_MANIFEST_IMPL}")
string(CONFIGURE "${NBL_MANIFEST_IMPL}" NBL_MANIFEST_IMPL_CONF)
file(WRITE "${NBL_CMAKE_COMPOMENT_OUTPUT_FILE}" "${NBL_MANIFEST_IMPL_CONF}")

# the reason behind this weird looking thing is you cannot nest bracket arguments https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#bracket-argument
# some variables need evaluation but some not and must be literals, to make this code read-able & work we do a small workaround
configure_file("${NBL_ROOT_PATH}/cmake/cpack/find/compoment/template.cmake" "${NBL_CMAKE_COMPOMENT_OUTPUT_FILE}.tmp" @ONLY)
file(READ "${NBL_CMAKE_COMPOMENT_OUTPUT_FILE}.tmp" _NBL_COMPOMENT_INCLUDE_LIST_TRANFORM_)
file(REMOVE "${NBL_CMAKE_COMPOMENT_OUTPUT_FILE}.tmp")
file(APPEND "${NBL_CMAKE_COMPOMENT_OUTPUT_FILE}" "\n${_NBL_COMPOMENT_INCLUDE_LIST_TRANFORM_}")
]=]
)

install(CODE "${NBL_FIND_NABLA_IMPL}" COMPONENT ${_COMPOMENT_})
endfunction()

# Generate compoment configurations
NBL_GEN_FIND_NABLA_CODE_IMPL(Headers include)
NBL_GEN_FIND_NABLA_CODE_IMPL(Libraries lib)
NBL_GEN_FIND_NABLA_CODE_IMPL(Runtimes runtime)
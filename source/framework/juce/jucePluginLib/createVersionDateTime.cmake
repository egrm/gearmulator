string(TIMESTAMP versionDate "%b %d %Y")
string(TIMESTAMP versionTime "%H:%M")

configure_file(${CMAKE_CURRENT_LIST_DIR}/versionDateTime.h.in ${TUS_VERSION_DATETIME_HEADER} @ONLY)

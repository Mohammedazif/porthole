# Expose the key proved during AirPlay Pair Verify to Porthole's admission
# callback. The submodule stays pinned and clean. A changed upstream hook must
# be reviewed before building an updated dependency.
set(_verified_dir "${_uxplay_bin}/verified")
file(MAKE_DIRECTORY "${_verified_dir}")
set(_upstream_hook "raop->callbacks.report_client_request(raop->callbacks.cls, deviceID, model, name, &admit_client);")
set(_verified_hook [=[
            char verified_id[69];
            char legacy_id[129];
            const char *identity = porthole_verified_airplay_key(conn->session, verified_id)
                ? verified_id : NULL;
            if (!identity && deviceID && *deviceID) {
                snprintf(legacy_id, sizeof legacy_id, "legacy:%.121s", deviceID);
                identity = legacy_id;
            }
            raop->callbacks.report_client_request(raop->callbacks.cls, (char *)identity,
                                                  model, name, &admit_client);
]=])
foreach(_upstream_file raop.c raop_handlers.h pairing.c)
    set(_upstream_path "${UXPLAY_DIR}/lib/${_upstream_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_upstream_path}")
    file(READ "${_upstream_path}" _copy)
    if(_upstream_file STREQUAL "raop_handlers.h")
        string(FIND "${_copy}" "${_upstream_hook}" _hook_position)
        if(_hook_position LESS 0)
            message(FATAL_ERROR "Review the Porthole admission hook for this version of UxPlay")
        endif()
        string(REPLACE "${_upstream_hook}" "${_verified_hook}" _copy "${_copy}")
    elseif(_upstream_file STREQUAL "raop.c")
        string(REPLACE "#include \"raop_handlers.h\""
            "extern bool porthole_verified_airplay_key(pairing_session_t *session, char out[69]);\n#include \"raop_handlers.h\""
            _copy "${_copy}")
    else()
        string(APPEND _copy [=[

/* Only a completed Pair Verify handshake proves that the live peer owns
 * this key. The display name and RTSP device ID do not provide that proof. */
bool porthole_verified_airplay_key(pairing_session_t *session, char out[69])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char key[ED25519_KEY_SIZE];
    if (!session || session->status != STATUS_FINISHED || !session->ed_theirs)
        return false;
    ed25519_key_get_raw(key, session->ed_theirs);
    memcpy(out, "key:", 4);
    for (size_t i = 0; i < sizeof key; i++) {
        out[4 + i * 2] = hex[key[i] >> 4];
        out[5 + i * 2] = hex[key[i] & 15];
    }
    out[68] = 0;
    return true;
}
]=])
    endif()
    # configure_file(COPYONLY) avoids rebuilding generated sources whose contents match.
    file(WRITE "${_verified_dir}/${_upstream_file}.source" "${_copy}")
    configure_file("${_verified_dir}/${_upstream_file}.source"
                   "${_verified_dir}/${_upstream_file}" COPYONLY)
endforeach()

get_target_property(_uxplay_verified_sources airplay SOURCES)
list(FILTER _uxplay_verified_sources EXCLUDE REGEX "(^|/)(raop|pairing)\\.c$")
set_property(TARGET airplay PROPERTY SOURCES ${_uxplay_verified_sources}
    "${_verified_dir}/raop.c" "${_verified_dir}/pairing.c")
target_include_directories(airplay PRIVATE "${UXPLAY_DIR}/lib"
    "${UXPLAY_DIR}/lib/llhttp" "${UXPLAY_DIR}/lib/playfair")

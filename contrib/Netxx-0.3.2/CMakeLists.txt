add_library(Netxx STATIC
    include/Netxx/Address.h
    include/Netxx/Datagram.h
    include/Netxx/DatagramServer.h
    include/Netxx/Netbuf.h
    include/Netxx/Peer.h
    include/Netxx/Probe.h
    include/Netxx/ProbeInfo.h
    include/Netxx/SockOpt.h
    include/Netxx/Stream.h
    include/Netxx/StreamBase.h
    include/Netxx/StreamServer.h
    include/Netxx/Types.h
    src/Accept.h
    src/common.h
    src/compat.h
    src/inet6.h
    src/OSError.h
    src/Probe_impl.h
    src/RecvFrom.h
    src/Resolve.h
    src/ServerBase.h
    src/SockAddr.h
    src/Socket.h
    src/TLS_Context_pimpl.h
    src/TLS_tls_pimpl.h

    src/Accept.cxx
    src/Address.cxx
    src/Datagram.cxx
    src/DatagramServer.cxx
    src/inet6.cxx
    src/OSError.cxx
    src/Peer.cxx
    src/Probe.cxx
    src/Probe_select.cxx
    src/RecvFrom.cxx
    src/Resolve_gethostbyname.cxx
    src/Resolve_getservbyname.cxx
    src/ServerBase.cxx
    src/SockAddr.cxx
    src/Socket.cxx
    src/SockOpt.cxx
    src/Stream.cxx
    src/StreamBase.cxx
    src/StreamServer.cxx
)
set_target_properties(Netxx PROPERTIES FOLDER contrib)
target_include_directories(Netxx PRIVATE include)
target_include_directories(Netxx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_compile_definitions(NETXX_NO_INET6)

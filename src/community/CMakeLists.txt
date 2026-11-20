add_library(cd_community STATIC Sha256.cpp Transport.cpp OfflineCache.cpp CommunityClient.cpp)
target_link_libraries(cd_community PUBLIC cd_state)
cd_core_target(cd_community)

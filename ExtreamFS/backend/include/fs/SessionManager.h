#pragma once
#include <string>

struct Session {
    bool active = false;
    std::string user;
    std::string group;
    std::string partitionId;
};

class SessionManager {
public:
    static Session currentSession;

    static bool Login(const std::string& user,
                      const std::string& pass,
                      const std::string& id,
                      std::string& outMsg);

    static void Logout();
};
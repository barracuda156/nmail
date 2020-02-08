// main.cpp
//
// Copyright (c) 2019-2020 Kristofer Berggren
// All rights reserved.
//
// nmail is distributed under the MIT license, see LICENSE for details.

#include <iostream>
#include <memory>

#include "apathy/path.hpp"

#include "addressbook.h"
#include "aes.h"
#include "config.h"
#include "imapmanager.h"
#include "lockfile.h"
#include "log.h"
#include "loghelp.h"
#include "serialized.h"
#include "smtpmanager.h"
#include "ui.h"
#include "util.h"

static bool ValidateConfig(const std::string& p_User, const std::string& p_Imaphost,
                           const uint16_t p_Imapport, const std::string& p_Smtphost,
                           const uint16_t p_Smtpport);
static bool ValidatePass(const std::string& p_Pass, const std::string& p_ErrorPrefix);
static bool ReportConfigError(const std::string& p_Param);
static void ShowHelp();
static void ShowVersion();
static void SetupCommon(std::shared_ptr<Config> p_Config);
static void SetupGmail(std::shared_ptr<Config> p_Config);
static void SetupOutlook(std::shared_ptr<Config> p_Config);

int main(int argc, char* argv[])
{
  // Defaults
  umask(S_IRWXG | S_IRWXO);
  Util::SetApplicationDir(std::string(getenv("HOME")) + std::string("/.nmail"));
  bool online = true;
  std::string setup;
  
  // Argument handling
  std::vector<std::string> args(argv + 1, argv + argc);
  for (auto it = args.begin(); it != args.end(); ++it)
  {
    if (((*it == "-d") || (*it == "--configdir")) && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      Util::SetApplicationDir(*it);
    }
    else if ((*it == "-e") || (*it == "--verbose"))
    {
      Log::SetVerboseLevel(Log::DEBUG_LEVEL);
    }
    else if ((*it == "-ee") || (*it == "--extraverbose"))
    {
      Log::SetVerboseLevel(Log::TRACE_LEVEL);
    }
    else if ((*it == "-h") || (*it == "--help"))
    {
      ShowHelp();
      return 0;
    }
    else if ((*it == "-o") || (*it == "--offline"))
    {
      online = false;
    }
    else if (((*it == "-s") || (*it == "--setup")) && (std::distance(it + 1, args.end()) > 0))
    {
      ++it;
      setup = *it;
    }
    else if ((*it == "-v") || (*it == "--version"))
    {
      ShowVersion();
      return 0;
    }
    else
    {
      ShowHelp();
      return 1;
    }
  }
 
  if (!apathy::Path(Util::GetApplicationDir()).exists())
  {
    apathy::Path::makedirs(Util::GetApplicationDir());
  }

  DirLock dirLock(Util::GetApplicationDir());
  if (!dirLock.IsLocked())
  {
    std::cout <<
      "error: unable to acquire lock for " << Util::GetApplicationDir() << "\n" <<
      "       only one nmail session per account/confdir is supported.\n";
    return 1;
  }

  const std::string& logPath = Util::GetApplicationDir() + std::string("log.txt");
  Log::SetPath(logPath);

  Util::RegisterSignalHandler();
  
  const std::string version = Util::GetAppVersion();
  LOG_INFO("starting nmail %s", version.c_str());

  const std::string os = Util::GetOs();
  const std::string compiler = Util::GetCompiler();
  LOG_INFO("built using %s/%s", os.c_str(), compiler.c_str());

  Util::InitTempDir();

  const std::map<std::string, std::string> defaultConfig =
  {
    {"name", ""},
    {"address", ""},
    {"user", ""},
    {"pass", ""},
    {"imap_host", ""},
    {"imap_port", "993"},
    {"smtp_host", ""},
    {"smtp_port", "465"},
    {"smtp_user", ""},
    {"smtp_pass", ""},
    {"save_pass", "0"},
    {"inbox", "INBOX"},
    {"trash", ""},
    {"drafts", ""},
    {"sent", ""},
    {"client_store_sent", "0"},
    {"cache_encrypt", "1"},
    {"html_convert_cmd", Util::GetDefaultHtmlConvertCmd()},
    {"ext_viewer_cmd", Util::GetDefaultExtViewerCmd()},
    {"prefetch_level", "2"},
    {"verbose_logging", "0"},
  };
  const std::string configPath(Util::GetApplicationDir() + std::string("main.conf"));

  std::shared_ptr<Config> config = std::make_shared<Config>(configPath, defaultConfig);

  if (!setup.empty())
  {
    remove(configPath.c_str());
    config = std::make_shared<Config>(configPath, defaultConfig);
    
    if (setup == "gmail")
    {
      SetupGmail(config);
      config->Save();
    }
    else if (setup == "outlook")
    {
      SetupOutlook(config);
      config->Save();
    }
    else
    {
      std::cout << "error: unsupported email service \"" << setup << "\".\n\n";
      ShowHelp();
      return 1;
    }
  }

  // Read config
  const std::string& name = config->Get("name");
  const std::string& address = config->Get("address");
  const std::string& user = config->Get("user");
  std::string encPass = config->Get("pass");
  const std::string& imapHost = config->Get("imap_host");
  const std::string& smtpHost = config->Get("smtp_host");
  std::string smtpUser = config->Get("smtp_user");
  std::string encSmtpPass = config->Get("smtp_pass");
  const bool savePass = (config->Get("save_pass") == "1");
  const std::string& inbox = config->Get("inbox");
  std::string trash = config->Get("trash");
  std::string drafts = config->Get("drafts");
  std::string sent = config->Get("sent");
  const bool clientStoreSent = (config->Get("client_store_sent") == "1");
  const bool cacheEncrypt = (config->Get("cache_encrypt") == "1");
  Util::SetHtmlConvertCmd(config->Get("html_convert_cmd"));
  Util::SetExtViewerCmd(config->Get("ext_viewer_cmd"));

  if (Log::GetVerboseLevel() == Log::INFO_LEVEL)
  {
    if (config->Get("verbose_logging") == "1")
    {
      Log::SetVerboseLevel(Log::DEBUG_LEVEL);
    }
    else if (config->Get("verbose_logging") == "2")
    {
      Log::SetVerboseLevel(Log::TRACE_LEVEL);
    }
  }
  
  uint16_t imapPort = 0;
  uint16_t smtpPort = 0;
  uint32_t prefetchLevel = 0;
  try
  {
    imapPort = std::stoi(config->Get("imap_port"));
    smtpPort = std::stoi(config->Get("smtp_port"));
    prefetchLevel = std::stoi(config->Get("prefetch_level"));
  }
  catch (...)
  {
  }

  if (!ValidateConfig(user, imapHost, imapPort, smtpHost, smtpPort))
  {
    ShowHelp();
    return 1;
  }

  std::string pass;
  if (encPass.empty())
  {
    std::cout << (smtpUser.empty() ? "Password: " : "IMAP Password: ");
    pass = Util::GetPass();
    if (savePass)
    {
      encPass = Serialized::ToHex(AES::Encrypt(pass, user));
      config->Set("pass", encPass);
    }
  }
  else
  {
    pass = AES::Decrypt(Serialized::FromHex(encPass), user);
  }

  if (!ValidatePass(pass, smtpUser.empty() ? "" : "IMAP "))
  {
    return 1;
  }

  std::string smtpPass;
  if (smtpUser.empty())
  {
    smtpUser = user;
    smtpPass = pass;
  }
  else
  {
    if (encSmtpPass.empty())
    {
      std::cout << "SMTP Password: ";
      smtpPass = Util::GetPass();
      if (savePass)
      {
        encSmtpPass = Serialized::ToHex(AES::Encrypt(smtpPass, smtpUser));
        config->Set("smtp_pass", encSmtpPass);
      }
    }
    else
    {
      smtpPass = AES::Decrypt(Serialized::FromHex(encSmtpPass), smtpUser);
    }
  }
  
  if (!ValidatePass(smtpPass, "SMTP "))
  {
    return 1;
  }
  
  Util::InitStdErrRedirect(logPath);

  Ui ui(inbox, address, prefetchLevel);

  std::shared_ptr<ImapManager> imapManager =
    std::make_shared<ImapManager>(user, pass, imapHost, imapPort, online, cacheEncrypt,
                                  std::bind(&Ui::ResponseHandler, std::ref(ui), std::placeholders::_1, std::placeholders::_2),
                                  std::bind(&Ui::ResultHandler, std::ref(ui), std::placeholders::_1, std::placeholders::_2),
                                  std::bind(&Ui::StatusHandler, std::ref(ui), std::placeholders::_1));

  std::shared_ptr<SmtpManager> smtpManager =
    std::make_shared<SmtpManager>(smtpUser, smtpPass, smtpHost, smtpPort, name, address, online,
                                  std::bind(&Ui::SmtpResultHandler, std::ref(ui), std::placeholders::_1),
                                  std::bind(&Ui::StatusHandler, std::ref(ui), std::placeholders::_1));

  AddressBook::Init(cacheEncrypt, pass);

  ui.SetImapManager(imapManager);
  ui.SetTrashFolder(trash);
  ui.SetDraftsFolder(drafts);
  ui.SetSentFolder(sent);
  ui.SetClientStoreSent(clientStoreSent);
  ui.SetSmtpManager(smtpManager);

  ui.Run();

  ui.ResetSmtpManager();
  ui.ResetImapManager();

  smtpManager.reset();
  imapManager.reset();

  config->Save();
  config.reset();

  AddressBook::Cleanup();
  
  Util::CleanupTempDir();

  Util::CleanupStdErrRedirect();
  
  LOG_INFO("exiting nmail");
  
  return 0;  
}

static void ShowHelp()
{
  std::cout <<
    "nmail is a console-based email client with a user interface similar to\n" 
    "alpine / pine, supporting IMAP and SMTP.\n"
    "\n"
    "Usage: nmail [OPTION]\n"
    "\n"
    "Options:\n"
    "   -d, --confdir <DIR>  use a different directory than ~/.nmail\n"
    "   -e, --verbose        enable verbose logging\n"
    "   -ee, --extraverbose  enable extra verbose logging\n"
    "   -h, --help           display this help and exit\n"
    "   -o, --offline        run in offline mode\n"
    "   -s, --setup <SERV>   setup wizard for specified service, supported\n"
    "                        services: gmail, outlook\n"
    "   -v, --version        output version information and exit\n"
    "\n"
    "Examples:\n"
    "   nmail -s gmail       setup nmail for a gmail account\n"
    "\n"
    "Files:\n"
    "   ~/.nmail/main.conf   configures mail account and general setings.\n"
    "   ~/.nmail/ui.conf     customizes UI settings.\n"
    "\n"
    "Report bugs at https://github.com/d99kris/nmail\n"
    "\n";
}

static void ShowVersion()
{
  std::cout <<
    "nmail " << Util::GetAppVersion() << "\n"
    "\n"
    "Copyright (c) 2019-2020 Kristofer Berggren\n"
    "\n"
    "nmail is distributed under the MIT license.\n"
    "\n"
    "Written by Kristofer Berggren.\n";
}

static void SetupCommon(std::shared_ptr<Config> p_Config)
{
  Util::RmDir(Util::GetApplicationDir() + std::string("cache"));

  std::string email;
  std::cout << "Email: ";
  std::getline(std::cin, email);
  std::cout << "Name: ";
  std::string name;
  std::getline(std::cin, name);
  std::cout << "Save password (y/n): ";
  std::string savepass;
  std::getline(std::cin, savepass);

  p_Config->Set("name", name);
  p_Config->Set("address", email);
  p_Config->Set("user", email);
  p_Config->Set("cache_encrypt", "1");  
  p_Config->Set("save_pass", std::to_string((int)(savepass == "y")));
}

static void SetupGmail(std::shared_ptr<Config> p_Config)
{
  SetupCommon(p_Config);
  
  p_Config->Set("imap_host", "imap.gmail.com");
  p_Config->Set("imap_port", "993");
  p_Config->Set("smtp_host", "smtp.gmail.com");
  p_Config->Set("smtp_port", "465");
  p_Config->Set("inbox", "INBOX");  
  p_Config->Set("trash", "[Gmail]/Trash");
  p_Config->Set("drafts", "[Gmail]/Drafts");
  p_Config->Set("sent", "[Gmail]/Sent Mail");
}

static void SetupOutlook(std::shared_ptr<Config> p_Config)
{
  SetupCommon(p_Config);

  p_Config->Set("imap_host", "imap-mail.outlook.com");
  p_Config->Set("imap_port", "993");
  p_Config->Set("smtp_host", "smtp-mail.outlook.com");
  p_Config->Set("smtp_port", "587");
  p_Config->Set("inbox", "Inbox");  
  p_Config->Set("trash", "Deleted");  
  p_Config->Set("drafts", "Drafts");
  p_Config->Set("sent", "Sent");
}

bool ValidateConfig(const std::string& p_User, const std::string& p_Imaphost,
                    const uint16_t p_Imapport, const std::string& p_Smtphost,
                    const uint16_t p_Smtpport)
{
  if (p_User.empty()) return ReportConfigError("user");
  if (p_Imaphost.empty()) return ReportConfigError("imaphost");
  if (p_Imapport == 0) return ReportConfigError("imapport");
  if (p_Smtphost.empty()) return ReportConfigError("smtphost");
  if (p_Smtpport == 0) return ReportConfigError("smtpport");

  return true;
}

bool ValidatePass(const std::string& p_Pass, const std::string& p_ErrorPrefix)
{
  if (p_Pass.empty())
  {
    std::cout << "error: " << p_ErrorPrefix << "pass not specified.\n\n";
    return false;
  }

  return true;
}

bool ReportConfigError(const std::string& p_Param)
{
  const std::string configPath(Util::GetApplicationDir() + std::string("main.conf"));
  std::cout << "error: " << p_Param << " not specified in config file (" << configPath
            << ").\n\n";
  return false;
}

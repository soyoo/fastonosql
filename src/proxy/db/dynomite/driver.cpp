/*  Copyright (C) 2014-2019 FastoGT. All right reserved.

    This file is part of FastoNoSQL.

    FastoNoSQL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoNoSQL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoNoSQL. If not, see <http://www.gnu.org/licenses/>.
*/

#include "proxy/db/dynomite/driver.h"

#include <common/convert2string.h>           // for ConvertFromString, etc
#include <common/file_system/file_system.h>  // for copy_file

#include <fastonosql/core/db/dynomite/db_connection.h>  // for DBConnection, INFO_REQUEST, etc
#include <fastonosql/core/value.h>

#include "proxy/command/command.h"  // for CreateCommand, etc
#include "proxy/command/command_logger.h"
#include "proxy/db/dynomite/command.h"              // for Command
#include "proxy/db/dynomite/connection_settings.h"  // for ConnectionSettings

#define REDIS_TYPE_COMMAND "TYPE"
#define REDIS_SHUTDOWN_COMMAND "SHUTDOWN"
#define REDIS_BACKUP_COMMAND "SAVE"
#define REDIS_SET_PASSWORD_COMMAND "CONFIG SET requirepass"
#define REDIS_SET_MAX_CONNECTIONS_COMMAND "CONFIG SET maxclients"
#define REDIS_GET_PROPERTY_SERVER_COMMAND "CONFIG GET *"
#define REDIS_PUBSUB_CHANNELS_COMMAND "PUBSUB CHANNELS"
#define REDIS_PUBSUB_NUMSUB_COMMAND "PUBSUB NUMSUB"

#define REDIS_SET_DEFAULT_DATABASE_COMMAND_1ARGS_S "SELECT %s"

#define BACKUP_DEFAULT_PATH "/var/lib/redis/dump.rdb"
#define EXPORT_DEFAULT_PATH "/var/lib/redis/dump.rdb"

namespace {

common::Value::Type ConvertFromStringRType(const fastonosql::core::command_buffer_t& type) {
  if (type.empty()) {
    return common::Value::TYPE_NULL;
  }

  if (type == GEN_CMD_STRING("string")) {
    return common::Value::TYPE_STRING;
  } else if (type == GEN_CMD_STRING("list")) {
    return common::Value::TYPE_ARRAY;
  } else if (type == GEN_CMD_STRING("set")) {
    return common::Value::TYPE_SET;
  } else if (type == GEN_CMD_STRING("hash")) {
    return common::Value::TYPE_HASH;
  } else if (type == GEN_CMD_STRING("zset")) {
    return common::Value::TYPE_ZSET;
  }
  return common::Value::TYPE_NULL;
}

}  // namespace

namespace fastonosql {
namespace proxy {
namespace dynomite {

Driver::Driver(IConnectionSettingsBaseSPtr settings)
    : IDriverRemote(settings), impl_(new core::dynomite::DBConnection(this)) {
  COMPILE_ASSERT(core::dynomite::DBConnection::GetConnectionType() == core::DYNOMITE,
                 "DBConnection must be the same type as Driver!");
  CHECK(GetType() == core::DYNOMITE);
}

Driver::~Driver() {
  delete impl_;
}

bool Driver::IsInterrupted() const {
  return impl_->IsInterrupted();
}

void Driver::SetInterrupted(bool interrupted) {
  return impl_->SetInterrupted(interrupted);
}

core::translator_t Driver::GetTranslator() const {
  return impl_->GetTranslator();
}

bool Driver::IsConnected() const {
  return impl_->IsConnected();
}

bool Driver::IsAuthenticated() const {
  return impl_->IsAuthenticated();
}

void Driver::InitImpl() {}

void Driver::ClearImpl() {}

core::FastoObjectCommandIPtr Driver::CreateCommand(core::FastoObject* parent,
                                                   const core::command_buffer_t& input,
                                                   core::CmdLoggingType logging_type) {
  return proxy::CreateCommand<Command>(parent, input, logging_type);
}

core::FastoObjectCommandIPtr Driver::CreateCommandFast(const core::command_buffer_t& input,
                                                       core::CmdLoggingType logging_type) {
  return proxy::CreateCommandFast<Command>(input, logging_type);
}

core::IDataBaseInfoSPtr Driver::CreateDatabaseInfo(const core::db_name_t& name, bool is_default, size_t size) {
  return core::IDataBaseInfoSPtr(impl_->MakeDatabaseInfo(name, is_default, size));
}

common::Error Driver::SyncConnect() {
  auto dynomite_redis_settings = GetSpecificSettings<ConnectionSettings>();
  core::dynomite::RConfig rconf(dynomite_redis_settings->GetInfo(), dynomite_redis_settings->GetSSHInfo());
  return impl_->Connect(rconf);
}

common::Error Driver::SyncDisconnect() {
  return impl_->Disconnect();
}

common::Error Driver::ExecuteImpl(const core::command_buffer_t& command, core::FastoObject* out) {
  return impl_->Execute(command, out);
}

common::Error Driver::DBkcountImpl(core::keys_limit_t* size) {
  return impl_->DBKeysCount(size);
}

common::Error Driver::GetCurrentServerInfo(core::IServerInfo** info) {
  core::FastoObjectCommandIPtr cmd = CreateCommandFast(GEN_CMD_STRING(DB_INFO_COMMAND), core::C_INNER);
  common::Error err = Execute(cmd.get());
  if (err) {
    return err;
  }

  core::command_buffer_t content = common::ConvertToString(cmd.get());
  core::IServerInfo* linfo = impl_->MakeServerInfo(common::ConvertToString(content));  // #FIXME

  if (!linfo) {
    return common::make_error("Invalid " DB_INFO_COMMAND " command output");
  }

  *info = linfo;
  return common::Error();
}

common::Error Driver::GetServerCommands(std::vector<const core::CommandInfo*>* commands) {
  std::vector<const core::CommandInfo*> lcommands;
  const core::ConstantCommandsArray& origin = core::dynomite::DBConnection::GetCommands();
  for (size_t i = 0; i < origin.size(); ++i) {
    lcommands.push_back(&origin[i]);
  }
  *commands = lcommands;
  return common::Error();
}

common::Error Driver::GetCurrentDataBaseInfo(core::IDataBaseInfo** info) {
  if (!info) {
    DNOTREACHED();
    return common::make_error_inval();
  }

  return impl_->Select(impl_->GetCurrentDBName(), info);
}

void Driver::HandleBackupEvent(events::BackupRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::BackupResponseEvent::value_type res(ev->value());
  NotifyProgress(sender, 25);
  core::FastoObjectCommandIPtr cmd = CreateCommandFast(GEN_CMD_STRING(REDIS_BACKUP_COMMAND), core::C_INNER);
  common::Error err = Execute(cmd);
  if (err) {
    res.setErrorInfo(err);
  } else {
    common::ErrnoError err = common::file_system::copy_file(BACKUP_DEFAULT_PATH, res.path);
    if (err) {
      res.setErrorInfo(common::make_error_from_errno(err));
    }
  }
  NotifyProgress(sender, 75);
  Reply(sender, new events::BackupResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

void Driver::HandleRestoreEvent(events::RestoreRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::RestoreResponseEvent::value_type res(ev->value());
  NotifyProgress(sender, 25);
  common::ErrnoError err = common::file_system::copy_file(res.path, EXPORT_DEFAULT_PATH);
  if (err) {
    res.setErrorInfo(common::make_error_from_errno(err));
  }
  NotifyProgress(sender, 75);
  Reply(sender, new events::RestoreResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

void Driver::HandleLoadDatabaseContentEvent(events::LoadDatabaseContentRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::LoadDatabaseContentResponseEvent::value_type res(ev->value());
  const core::command_buffer_t pattern_result = core::GetKeysPattern(res.cursor_in, res.pattern, res.keys_count);
  core::FastoObjectCommandIPtr cmd = CreateCommandFast(pattern_result, core::C_INNER);
  NotifyProgress(sender, 50);
  common::Error err = Execute(cmd);
  if (err) {
    res.setErrorInfo(err);
  } else {
    core::FastoObject::childs_t rchildrens = cmd->GetChildrens();
    if (rchildrens.size()) {
      CHECK_EQ(rchildrens.size(), 1);
      core::FastoObject* array = rchildrens[0].get();
      CHECK(array);
      auto array_value = array->GetValue();
      common::ArrayValue* arm = nullptr;
      if (!array_value->GetAsList(&arm)) {
        goto done;
      }

      CHECK_EQ(arm->GetSize(), 2);
      core::cursor_t cursor;
      bool isok = arm->GetUInteger(0, &cursor);
      if (!isok) {
        goto done;
      }
      res.cursor_out = cursor;

      common::ArrayValue* ar = nullptr;
      isok = arm->GetList(1, &ar);
      if (!isok) {
        goto done;
      }

      std::vector<core::FastoObjectCommandIPtr> cmds;
      cmds.reserve(ar->GetSize() * 2);
      for (size_t i = 0; i < ar->GetSize(); ++i) {
        core::command_buffer_t key;
        bool isok = ar->GetString(i, &key);
        if (isok) {
          const core::nkey_t key_str(key);
          const core::NKey k(key_str);
          const core::NDbKValue dbv(k, core::NValue());
          core::command_buffer_writer_t wr_type;
          wr_type << REDIS_TYPE_COMMAND " " << key_str.GetForCommandLine();
          cmds.push_back(CreateCommandFast(wr_type.str(), core::C_INNER));

          core::command_buffer_writer_t wr_ttl;
          wr_ttl << DB_GET_TTL_COMMAND " " << key_str.GetForCommandLine();
          cmds.push_back(CreateCommandFast(wr_ttl.str(), core::C_INNER));
          res.keys.push_back(dbv);
        }
      }

      err = impl_->ExecuteAsPipeline(cmds, &LOG_COMMAND);
      if (err) {
        goto done;
      }

      for (size_t i = 0; i < res.keys.size(); ++i) {
        core::FastoObjectIPtr cmdType = cmds[i * 2];
        core::FastoObject::childs_t tchildrens = cmdType->GetChildrens();
        if (tchildrens.size()) {
          DCHECK_EQ(tchildrens.size(), 1);
          if (tchildrens.size() == 1) {
            core::command_buffer_t type_redis = tchildrens[0]->ToString();
            common::Value::Type ctype = ConvertFromStringRType(type_redis);
            core::NValue empty_val(core::CreateEmptyValueFromType(ctype));
            res.keys[i].SetValue(empty_val);
          }
        }

        core::FastoObjectIPtr cmdType2 = cmds[i * 2 + 1];
        tchildrens = cmdType2->GetChildrens();
        if (tchildrens.size()) {
          DCHECK_EQ(tchildrens.size(), 1);
          if (tchildrens.size() == 1) {
            auto vttl = tchildrens[0]->GetValue();
            core::ttl_t ttl = 0;
            if (vttl->GetAsLongLongInteger(&ttl)) {
              core::NKey key = res.keys[i].GetKey();
              key.SetTTL(ttl);
              res.keys[i].SetKey(key);
            }
          }
        }
      }

      err = DBkcountImpl(&res.db_keys_count);
      DCHECK(!err);
    }
  }
done:
  NotifyProgress(sender, 75);
  Reply(sender, new events::LoadDatabaseContentResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

void Driver::HandleLoadServerPropertyEvent(events::ServerPropertyInfoRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::ServerPropertyInfoResponseEvent::value_type res(ev->value());
  core::FastoObjectCommandIPtr cmd =
      CreateCommandFast(GEN_CMD_STRING(REDIS_GET_PROPERTY_SERVER_COMMAND), core::C_INNER);
  NotifyProgress(sender, 50);
  common::Error err = Execute(cmd);
  if (err) {
    res.setErrorInfo(err);
  } else {
    core::FastoObject::childs_t ch = cmd->GetChildrens();
    if (ch.size()) {
      CHECK_EQ(ch.size(), 1);
      core::FastoObject* array = ch[0].get();
      auto array_value = array->GetValue();
      common::ArrayValue* arr = nullptr;
      if (array_value->GetAsList(&arr)) {
        res.info = core::MakeServerProperty(arr);
      }
    }
  }
  NotifyProgress(sender, 75);
  Reply(sender, new events::ServerPropertyInfoResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

void Driver::HandleServerPropertyChangeEvent(events::ChangeServerPropertyInfoRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::ChangeServerPropertyInfoResponseEvent::value_type res(ev->value());

  NotifyProgress(sender, 50);
  core::command_buffer_writer_t wr;
  wr << "CONFIG SET " << res.new_item.first << " " << res.new_item.second;
  core::command_buffer_t change_request = wr.str();
  core::FastoObjectCommandIPtr cmd = CreateCommandFast(change_request, core::C_INNER);
  common::Error err = Execute(cmd);
  if (err) {
    res.setErrorInfo(err);
  } else {
    res.is_change = true;
  }
  NotifyProgress(sender, 75);
  Reply(sender, new events::ChangeServerPropertyInfoResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

void Driver::HandleLoadServerChannelsRequestEvent(events::LoadServerChannelsRequestEvent* ev) {
  QObject* sender = ev->sender();
  NotifyProgress(sender, 0);
  events::LoadServerChannelsResponseEvent::value_type res(ev->value());

  NotifyProgress(sender, 50);
  core::command_buffer_writer_t wr;
  wr << REDIS_PUBSUB_CHANNELS_COMMAND " " << res.pattern;
  const core::command_buffer_t load_channels_request = wr.str();
  core::FastoObjectCommandIPtr cmd = CreateCommandFast(load_channels_request, core::C_INNER);
  common::Error err = Execute(cmd);
  if (err) {
    res.setErrorInfo(err);
    goto done;
  } else {
    core::FastoObject::childs_t rchildrens = cmd->GetChildrens();
    if (rchildrens.size()) {
      CHECK_EQ(rchildrens.size(), 1);
      core::FastoObject* array = rchildrens[0].get();
      CHECK(array);
      auto array_value = array->GetValue();
      common::ArrayValue* arm = nullptr;
      if (!array_value->GetAsList(&arm) || !arm->GetSize()) {
        goto done;
      }

      std::vector<core::FastoObjectCommandIPtr> cmds;
      cmds.reserve(arm->GetSize());
      for (size_t i = 0; i < arm->GetSize(); ++i) {
        core::command_buffer_t channel;
        bool isok = arm->GetString(i, &channel);
        if (isok) {
          core::command_buffer_writer_t wr2;
          wr2 << REDIS_PUBSUB_NUMSUB_COMMAND " " << channel;
          proxy::NDbPSChannel c(core::ReadableString(channel), 0);
          cmds.push_back(CreateCommandFast(wr2.str(), core::C_INNER));
          res.channels.push_back(c);
        }
      }

      err = impl_->ExecuteAsPipeline(cmds, &LOG_COMMAND);
      if (err) {
        res.setErrorInfo(err);
        goto done;
      }

      for (size_t i = 0; i < res.channels.size(); ++i) {
        core::FastoObjectIPtr subCount = cmds[i];
        core::FastoObject::childs_t tchildrens = subCount->GetChildrens();
        if (tchildrens.size()) {
          DCHECK_EQ(tchildrens.size(), 1);
          if (tchildrens.size() == 1) {
            core::FastoObject* array_sub = tchildrens[0].get();
            auto arr_value = array_sub->GetValue();
            common::ArrayValue* array_sub_inner = nullptr;
            if (arr_value->GetAsList(&array_sub_inner)) {
              common::Value* fund_sub = nullptr;
              if (array_sub_inner->Get(1, &fund_sub)) {
                common::Value::Type t = fund_sub->GetType();
                if (t == common::Value::TYPE_LONG_LONG_INTEGER) {
                  long long lsub;
                  if (fund_sub->GetAsLongLongInteger(&lsub)) {
                    res.channels[i].SetNumberOfSubscribers(lsub);
                  }
                } else if (t == common::Value::TYPE_STRING) {
                  core::command_buffer_t lsub_str;
                  long long lsub;
                  if (fund_sub->GetAsString(&lsub_str) && common::ConvertFromBytes(lsub_str, &lsub)) {
                    res.channels[i].SetNumberOfSubscribers(lsub);
                  }
                }
              }
            }
          }
        }
      }
    }
  }

done:
  NotifyProgress(sender, 75);
  Reply(sender, new events::LoadServerChannelsResponseEvent(this, res));
  NotifyProgress(sender, 100);
}

core::IServerInfoSPtr Driver::MakeServerInfoFromString(const std::string& val) {
  return core::IServerInfoSPtr(impl_->MakeServerInfo(val));
}

}  // namespace dynomite
}  // namespace proxy
}  // namespace fastonosql

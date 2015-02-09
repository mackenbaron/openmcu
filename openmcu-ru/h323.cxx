
#include <ptlib.h>
#include <ptlib/video.h>

#include "version.h"
#include "mcu.h"
#include "h323.h"
#include "html.h"

#include <h323pdu.h>
#include <math.h>
#include <stdio.h>

#define new PNEW

#ifdef _WIN32
PluginLoaderStartup2  OpenMCU::pluginLoader;
H323PluginCodecManager * OpenMCU::plugmgr=NULL;
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

void SpliceMacro(PString & text, const PString & token, const PString & value)
{
  PRegularExpression RegEx("<?!--#status[ \t\r\n]+" + token + "[ \t\r\n]*-->?",
                           PRegularExpression::Extended|PRegularExpression::IgnoreCase);
  PINDEX pos, len;
  while (text.FindRegEx(RegEx, pos, len))
    text.Splice(value, pos, len);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

class MCUConnectionCleaner : public PThread
{
  public:
    MCUConnectionCleaner(MCUH323EndPoint * _ep, const PString & _callToken)
      : PThread(10000, AutoDeleteThread, NormalPriority, "MCU Connection Cleaner "+_callToken)
    {
      ep = _ep;
      callToken = _callToken;
      Resume();
    }

    void Main()
    {
      PTRACE(1, "MCU Connection cleaner start " << callToken);
      MCUConnectionList & connectionList = ep->GetConnectionList();
      MCUConnectionList & connectionDeleteList = ep->GetConnectionDeleteList();
      MCUConnectionList::shared_iterator r = connectionList.Find(callToken);
      if(r != connectionList.end())
      {
        MCUH323Connection *conn = *r;
        if(connectionList.Erase(r))
        {
          conn->CleanUpOnCallEnd();
          conn->OnCleared();
          delete conn;
          MCUConnectionList::shared_iterator s = connectionDeleteList.Find(callToken);
          if(s != connectionDeleteList.end())
            connectionDeleteList.Erase(s);
        }
      }
      PTRACE(1, "MCU Connection cleaner stop " << callToken);
    }

  protected:
    PString callToken;
    MCUH323EndPoint *ep;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323EndPoint::MCUH323EndPoint(ConferenceManager & _conferenceManager)
  : conferenceManager(_conferenceManager), connectionList(1024), connectionDeleteList(1024)
{
  rsCaps = NULL;
  tsCaps = NULL;
  rvCaps = NULL;
  tvCaps = NULL;
  cvCaps = NULL;
  listCaps = NULL;
  trace_section = "MCUH323EndPoint: ";

  connectionMonitor = new ConnectionMonitor(*this);

  gatekeeperRequestTimeout = PTimeInterval(1000);
  gatekeeperRequestRetries = 2;
  gatekeeperMonitor = NULL;

#if MCU_VIDEO
  terminalType = e_MCUWithAVMP;
  enableVideo  = TRUE;
  videoFrameRate = 10;
#else
  terminalType = e_MCUWithAudioMP;
#endif

#ifdef _WIN32
  // MFC applications are not at all plugin friendly
  // You need to manually add the plugins
  OpenMCU::Current().LoadPluginMgr();
  OpenMCU::Current().pluginLoader.OnStartup();
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323EndPoint::~MCUH323EndPoint()
{
  if(gatekeeperMonitor)
  {
    gatekeeperMonitor->terminate = TRUE;
    gatekeeperMonitor->WaitForTermination();
    delete gatekeeperMonitor;
    gatekeeperMonitor = NULL;
  }
  if(connectionMonitor)
  {
    connectionMonitor->running = FALSE;
    connectionMonitor->WaitForTermination();
    delete connectionMonitor;
    connectionMonitor = NULL;
  }

#ifdef _WIN32
  // You need to manually remove the plugins
  OpenMCU::Current().RemovePluginMgr();
  OpenMCU::Current().pluginLoader.OnShutdown();
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::Initialise(PConfig & cfg)
{
  InitialiseCapability();

#ifdef HAS_AEC
  SetAECEnabled(FALSE);
#endif

  // Listeners
  PString defaultInterface = "*:1720";
  H323TransportAddressArray interfaces;
  PStringArray interfacesArray = cfg.GetString(InterfaceKey).Tokenise(",");
  BOOL interfacesEmpty = TRUE;
  for (int i = 0; i < interfacesArray.GetSize(); i++)
  {
    if(interfacesArray[i] != "")
    {
      interfacesEmpty = FALSE;
      interfaces.Append(new H323TransportAddress(interfacesArray[i]));
    }
  }
  if (interfacesArray.GetSize() == 0 || interfacesEmpty)
  {
    interfaces.Append(new H323TransportAddress(defaultInterface));
    cfg.SetString(InterfaceKey, defaultInterface);
  }
  StartListeners(interfaces);

  if (listeners.IsEmpty()) {
    PSYSTEMLOG(Fatal, "Main\tCould not open H.323 Listener");
  }

  // NAT Router IP
  PString nat_ip = cfg.GetString(NATRouterIPKey);
  if (nat_ip.Trim().IsEmpty()) {
    behind_masq = FALSE;
  } else {
    masqAddressPtr = new PIPSocket::Address(nat_ip);
    behind_masq = TRUE;
    PTRACE(2, trace_section << "Masquerading as address " << *(masqAddressPtr));
  }

  nat_lag_ip = cfg.GetString(NATTreatAsGlobalKey);
  if(!nat_lag_ip.IsEmpty())
  {
    nat_lag_ip=","+nat_lag_ip+",";
    nat_lag_ip.Replace(" ","", TRUE, 0);
  }

  // RTP Port Setup
  unsigned rtpPortBase = MCUConfig("Parameters").GetInteger(RTPPortBaseKey, 0);
  unsigned rtpPortMax = MCUConfig("Parameters").GetInteger(RTPPortMaxKey, 0);
  if((rtpPortBase>65532)||(rtpPortBase==0)) rtpPortBase=5000;
  if(rtpPortMax<=rtpPortBase) rtpPortMax=PMIN(rtpPortBase+5000,65532);
  SetRtpIpPorts(rtpPortBase, rtpPortMax);

  // Enable/Disable Fast Start & H.245 Tunneling
  BOOL disableFastStart = cfg.GetBoolean(DisableFastStartKey, TRUE);
  BOOL disableH245Tunneling = cfg.GetBoolean(DisableH245TunnelingKey, FALSE);
  DisableFastStart(disableFastStart);
  DisableH245Tunneling(disableH245Tunneling);

  // MCU Server Id
  // SetLocalUserName make localAliasNames.RemoveAll() !!!
  SetLocalUserName(OpenMCU::Current().GetServerId());

  // Gatekeeper UserName
  PString gkUserName = cfg.GetString(GatekeeperUserNameKey,"MCU");
  AddAliasName(gkUserName);

  // Gatekeeper password
  PString gkPassword = PHTTPPasswordField::Decrypt(cfg.GetString(GatekeeperPasswordKey));
  SetGatekeeperPassword(gkPassword);

  // Gatekeeper Alias
  PStringArray aliasesArray = cfg.GetString(GatekeeperAliasKey).Tokenise(",");
  for(PINDEX i = 0; i < aliasesArray.GetSize(); i++)
  {
    PString alias = aliasesArray[i];
    AddAliasName(alias); // Add the alias to the endpoint aliaslist
  }

  // Gatekeeper Time To Live
  registrationTimeToLive = cfg.GetString(GatekeeperTTLKey);

  // GatekeeperMonitor
  PString gkMode = cfg.GetString(GatekeeperModeKey, "No gatekeeper");
  if(gkMode == "No gatekeeper")
  {
    if(gatekeeperMonitor)
    {
      gatekeeperMonitor->terminate = TRUE;
      gatekeeperMonitor->WaitForTermination();
      delete gatekeeperMonitor;
      gatekeeperMonitor = NULL;
    }
  }
  else
  {
    unsigned gkRetryInterval = cfg.GetInteger(GatekeeperRetryIntervalKey, 30);
    if(!gatekeeperMonitor)
      gatekeeperMonitor = new GatekeeperMonitor(*this, gkMode, gkRetryInterval*1000);
  }

}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::HasListener(PString host, PString port)
{
  PString address = "ip$"+host+":"+port;
  for(PINDEX i = 0; i < listeners.GetSize(); i++)
  {
    if(listeners[i].GetTransportAddress().Find(address) == 0)
      return TRUE;
  }
  return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

H323Connection * MCUH323EndPoint::CreateConnection(unsigned callReference, void * userData, H323Transport * transport, H323SignalPDU *)
{
  MCUH323Connection *conn = new MCUH323Connection(*this, callReference, userData);
  if(gatekeeper)
  {
    MCUH323Gatekeeper *mcu_gk = dynamic_cast<MCUH323Gatekeeper *>(gatekeeper);
    if(mcu_gk != NULL)
    {
      PString remote_host = transport->GetRemoteAddress().GetHostName();
      if(remote_host != "*" && remote_host != GetGatekeeperHostName())
        mcu_gk->AddIgnoreConnection(callReference, conn);
    }
  }
  return conn;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323Connection * MCUH323EndPoint::FindConnectionWithoutLock(const PString & token)
{
  MCUConnectionList::shared_iterator it = connectionList.Find(token);
  if(it != connectionList.end())
  {
    MCUH323Connection *conn = *it;
    if(conn->IsClearing() || conn->IsShuttingDown())
      return NULL;
    return it.GetObject();
  }
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323Connection * MCUH323EndPoint::FindConnectionWithLock(const PString & token)
{
  MCUConnectionList::shared_iterator it = connectionList.Find(token);
  if(it != connectionList.end())
  {
    MCUH323Connection *conn = *it;
    if(conn->IsClearing() || conn->IsShuttingDown())
      return NULL;
    return it.GetCapturedObject();
  }
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::HasConnection(const PString & token)
{
  return FindConnectionWithoutLock(token) != NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::ClearCall(const PString & callToken, H323Connection::CallEndReason reason)
{
  return ClearCallSynchronous(callToken, reason, NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::ClearCallSynchronous(const PString & callToken, H323Connection::CallEndReason reason, PSyncPoint * sync)
{
  // H.323 cleaner thread
  if(PThread::Current() == (PThread *)connectionsCleaner)
  {
    PTRACE(2, trace_section << "Found H.323 cleaner thread " << callToken << ", redirection to H323EndPoint");
    return H323EndPoint::ClearCallSynchronous(callToken, reason, sync);
  }

  MCUH323Connection *connection = NULL;
  MCUConnectionList::shared_iterator it = connectionList.Find(callToken);
  if(it != connectionList.end())
  {
    connection = *it;
    if(!connection->SetClearing())
    {
      PTRACE(2, trace_section << "Cleaner process already running " << callToken << " thread " << PThread::Current()->GetThreadName());
      return TRUE;
    }
  }

  if(connection == NULL)
  {
    // Проверить в списке h323plus
    if(connectionsActive.GetAt(callToken) == NULL)
      return FALSE;
    // Перенаправить в h323plus
    PTRACE(1, trace_section << "Unknown connection " << callToken << ", redirection to H323EndPoint");
    return H323EndPoint::ClearCallSynchronous(callToken, reason, sync);
  }

  PTRACE(1, trace_section << "Clearing connection " << callToken << " reason=" << reason << " thread " << PThread::Current()->GetThreadName());

  connection->SetCallEndReason(reason, sync);

  if(connection->GetConnectionType() == CONNECTION_TYPE_H323)
  {
    // Удалить из списков h323plus
    connectionsMutex.Wait();
    connectionsActive.RemoveAt(callToken);
    connectionsToBeCleaned -= callToken;
    connectionsMutex.Signal();
  }

  // Добавить в список удаления
  connectionDeleteList.Insert(NULL, (long)connection, callToken);
  connectionDeleteList.Release((long)connection);

  // Поток удаления
  new MCUConnectionCleaner(this, callToken);

  if(sync != NULL)
    sync->Wait();

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::ClearAllCalls(H323Connection::CallEndReason reason, BOOL wait)
{
  connectionListMutex.Wait();
  for(MCUConnectionList::shared_iterator it = connectionList.begin(); it != connectionList.end(); ++it)
  {
    MCUH323Connection *conn = *it;
    conn->ClearCall(reason);
  }
  connectionListMutex.Signal();

  if(wait)
  {
    while(connectionDeleteList.GetCurrentSize() != 0)
      MCUTime::Sleep(10);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::OnConnectionCreated(MCUH323Connection * conn)
{
  PWaitAndSignal m(connectionListMutex);

  MCUConnectionList::shared_iterator it = connectionList.Find(conn->GetCallToken());
  if(it != connectionList.end())
  {
    // Повтор
    if(it.GetObject() == conn)
      return TRUE;
    // Удалить
    PTRACE(1, trace_section << "Error !");
    PString fakeToken = PGloballyUniqueID().AsString();
    connectionList.Insert(conn, (long)conn, fakeToken);
    connectionList.Release((long)conn);
    ClearCall(fakeToken);
    return FALSE;
  }

  PTRACE(1, trace_section << "Insert connection " << conn->GetCallToken());
  connectionList.Insert(conn, (long)conn, conn->GetCallToken());
  connectionList.Release((long)conn);

  Registrar *registrar = OpenMCU::Current().GetRegistrar();
  registrar->ConnectionCreated(conn->GetCallToken());

  if(conn->GetCallToken().Left(4) != "tcp:")
    connectionMonitor->AddConnection(conn->GetCallToken());

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::OnConnectionCleared(H323Connection & connection, const PString & callToken)
{
  connectionMonitor->RemoveConnection(callToken);

  Registrar *registrar = OpenMCU::Current().GetRegistrar();
  registrar->ConnectionCleared(callToken);

  if(gatekeeper)
  {
    MCUH323Gatekeeper *mcu_gk = dynamic_cast<MCUH323Gatekeeper *>(gatekeeper);
    if(mcu_gk != NULL)
      mcu_gk->RemoveIgnoreConnection(connection.GetCallReference());
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::CleanUpConnections()
{
  PTRACE(3, trace_section << "CleanUpConnections");
  return H323EndPoint::CleanUpConnections();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetGatekeeperHostName()
{
  PString host;
  if(gatekeeper)
    host = gatekeeper->GetTransport().GetRemoteAddress().GetHostName();
  return host;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

H323Gatekeeper * MCUH323EndPoint::CreateGatekeeper(H323Transport * transport)
{
  return new MCUH323Gatekeeper(*this, transport);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::InitialiseCapability()
{
  // Setup capabilities
  if(capabilities.GetSize() == 0)
  {
    PTRACE(3, trace_section << "Add all capabilities");
    H323CapabilityFactory::KeyList_T stdCaps = H323CapabilityFactory::GetKeyList();
    for(H323CapabilityFactory::KeyList_T::const_iterator r = stdCaps.begin(); r != stdCaps.end(); ++r)
    {
      PString name = *r;
      MCUCapability *capability = MCUCapability::Create(name);
      if(capability)
        AddCapability(capability);
    }
  }

  unsigned rsConfig=1, tsConfig=1, rvConfig=1, tvConfig=1, saConfig=1, svConfig=1;
  if(MCUConfig("RECEIVE_SOUND").GetKeys().GetSize() == 0) rsConfig = 0;
  if(MCUConfig("TRANSMIT_SOUND").GetKeys().GetSize() == 0) tsConfig = 0;
  if(MCUConfig("RECEIVE_VIDEO").GetKeys().GetSize() == 0) rvConfig = 0;
  if(MCUConfig("TRANSMIT_VIDEO").GetKeys().GetSize() == 0) tvConfig = 0;
  if(MCUConfig("SIP Audio").GetKeys().GetSize() == 0) saConfig = 0;
  if(MCUConfig("SIP Video").GetKeys().GetSize() == 0) svConfig = 0;

  for(PINDEX i = 0; i < capabilities.GetSize(); i++)
  {
    H323Capability *cap = &capabilities[i];
    PString capname = cap->GetFormatName();

    // H.323
    if(rsConfig == 0 && cap->GetMainType() == 0)
    {
      if(capname.Right(4) == "{sw}")
        MCUConfig("RECEIVE_SOUND").SetBoolean(capname, TRUE);
      else
        MCUConfig("RECEIVE_SOUND").SetBoolean(capname+"{sw}", TRUE);
    }
    if(tsConfig == 0 && cap->GetMainType() == 0)
      MCUConfig("TRANSMIT_SOUND").SetBoolean(capname, TRUE);
    if(rvConfig == 0 && cap->GetMainType() == 1)
      MCUConfig("RECEIVE_VIDEO").SetBoolean(capname, TRUE);
    if(tvConfig == 0 && cap->GetMainType() == 1)
      MCUConfig("TRANSMIT_VIDEO").SetBoolean(capname, TRUE);

    if(rsConfig == 1 && cap->GetMainType() == 0)
    {
      if(capname.Right(4) == "{sw}" && !MCUConfig("RECEIVE_SOUND").HasKey(capname))
        MCUConfig("RECEIVE_SOUND").SetBoolean(capname, TRUE);
      if(capname.Right(4) != "{sw}" && !MCUConfig("RECEIVE_SOUND").HasKey(capname+"{sw}"))
        MCUConfig("RECEIVE_SOUND").SetBoolean(capname+"{sw}", TRUE);
    }
    if(tsConfig == 1 && cap->GetMainType() == 0 && !MCUConfig("TRANSMIT_SOUND").HasKey(capname))
      MCUConfig("TRANSMIT_SOUND").SetBoolean(capname, TRUE);
    if(rvConfig == 1 && cap->GetMainType() == 1 && !MCUConfig("RECEIVE_VIDEO").HasKey(capname))
      MCUConfig("RECEIVE_VIDEO").SetBoolean(capname, TRUE);
    if(tvConfig == 1 && cap->GetMainType() == 1 && !MCUConfig("TRANSMIT_VIDEO").HasKey(capname))
      MCUConfig("TRANSMIT_VIDEO").SetBoolean(capname, TRUE);
    // SIP
    if(capname.Right(4) != "{sw}") capname += "{sw}";
    if(saConfig == 0 && cap->GetMainType() == 0)
      MCUConfig("SIP Audio").SetBoolean(capname, TRUE);
    if(svConfig == 0 && cap->GetMainType() == 1)
      MCUConfig("SIP Video").SetBoolean(capname, TRUE);
    if(saConfig == 1 && cap->GetMainType() == 0 && !MCUConfig("SIP Audio").HasKey(capname))
      MCUConfig("SIP Audio").SetBoolean(capname, TRUE);
    if(svConfig == 1 && cap->GetMainType() == 1 && !MCUConfig("SIP Video").HasKey(capname))
      MCUConfig("SIP Video").SetBoolean(capname, TRUE);
  }

  capabilities.RemoveAll();

  int capsNum = 5;
  capsNum += MCUConfig("RECEIVE_SOUND").GetKeys().GetSize()+
             MCUConfig("TRANSMIT_SOUND").GetKeys().GetSize()+
             MCUConfig("RECEIVE_VIDEO").GetKeys().GetSize()+
             MCUConfig("TRANSMIT_VIDEO").GetKeys().GetSize();
  rsCaps = (char **)calloc(capsNum,sizeof(char *));
  tsCaps = (char **)calloc(capsNum,sizeof(char *));
  rvCaps = (char **)calloc(capsNum,sizeof(char *));
  tvCaps = (char **)calloc(capsNum,sizeof(char *));
  listCaps = (char *)calloc(capsNum,64*sizeof(char));

  char buf[64];
  capsNum = 0;
  PStringList keys;
  keys = MCUConfig("RECEIVE_SOUND").GetKeys();
  for(PINDEX i = 0, j = 0; i < keys.GetSize(); i++)
  {
    if(MCUConfig("RECEIVE_SOUND").GetBoolean(keys[i]) != 1) continue;
    strcpy(buf, keys[i]);
    strcpy(&(listCaps[64*capsNum]),buf);
    rsCaps[j]=&(listCaps[64*capsNum]);
    j++; capsNum++;
  }

  keys = MCUConfig("TRANSMIT_SOUND").GetKeys();
  for(PINDEX i = 0, j = 0; i < keys.GetSize(); i++)
  {
    if(MCUConfig("TRANSMIT_SOUND").GetBoolean(keys[i]) != 1) continue;
    strcpy(buf, keys[i]);
    strcpy(&(listCaps[64*capsNum]),buf);
    tsCaps[j]=&(listCaps[64*capsNum]);
    j++; capsNum++;
  }

  keys = MCUConfig("RECEIVE_VIDEO").GetKeys();
  for(PINDEX i = 0, j = 0; i < keys.GetSize(); i++)
  {
    if(MCUConfig("RECEIVE_VIDEO").GetBoolean(keys[i]) != 1) continue;
    if(SkipCapability(keys[i])) continue;
    strcpy(buf, keys[i]);
    strcpy(&(listCaps[64*capsNum]),buf);
    rvCaps[j]=&(listCaps[64*capsNum]);
    j++; capsNum++;
  }

  keys = MCUConfig("TRANSMIT_VIDEO").GetKeys();
  for(PINDEX i = 0, j = 0; i < keys.GetSize(); i++)
  {
    if(MCUConfig("TRANSMIT_VIDEO").GetBoolean(keys[i]) != 1) continue;
    if(SkipCapability(keys[i], CONNECTION_TYPE_H323)) continue;
    strcpy(buf, keys[i]);
    strcpy(&(listCaps[64*capsNum]),buf);
    tvCaps[j]=&(listCaps[64*capsNum]);
    j++; capsNum++;
  }

  int listNum = 0;
  cout << "[RECEIVE_SOUND]= "; listNum=0; 
  while(rsCaps[listNum]!=NULL) { cout << rsCaps[listNum] << ", "; listNum++; }
  cout << "\n";
  cout << "[TRANSMIT_SOUND]= "; listNum=0; 
  while(tsCaps[listNum]!=NULL) { cout << tsCaps[listNum] << ", "; listNum++; }
  cout << "\n";
  cout << "[RECEIVE_VIDEO]= "; listNum=0; 
  while(rvCaps[listNum]!=NULL) { cout << rvCaps[listNum] << ", "; listNum++; }
  cout << "\n";
  cout << "[TRANSMIT_VIDEO]= "; listNum=0; 
  while(tvCaps[listNum]!=NULL) { cout << tvCaps[listNum] << ", "; listNum++; }
  cout << "\n";
  AddCapabilitiesMCU(0,0,(const char **)rsCaps);
  AddCapabilitiesMCU(0,1,(const char **)rvCaps);
  AddCapabilitiesMCU();
  cout << capabilities;

  PTRACE(2, trace_section << "Codecs (in preference order):\n" << setprecision(2) << GetCapabilities());;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::AddCapabilitiesMCU()
{
  // add fake VP8 capabilities, need only for H.323
  if(CheckCapability("VP8{sw}"))
  {
    for(int i = 0; vp8_resolutions[i].width != 0; ++i)
    {
      if(vp8_resolutions[i].width == 352) // skip default capability
        continue;
      MCUCapability *new_cap = MCUCapability::Create("VP8{sw}");
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      SetFormatParams(wf, vp8_resolutions[i].width, vp8_resolutions[i].height);
      AddCapability(new_cap);
    }
  }
  // add fake H.264 capabilities, need only for H.323
  if(CheckCapability("H.264{sw}"))
  {
    for(int i = 0; h264_profile_levels[i].level != 0; ++i)
    {
      if(h264_profile_levels[i].level_h241 == 29) // skip default capability
        continue;
      MCUCapability *new_cap = MCUCapability::Create("H.264{sw}");
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      wf.SetOptionInteger("Generic Parameter 42", h264_profile_levels[i].level_h241);
      wf.SetOptionInteger(OPTION_MAX_BIT_RATE, h264_profile_levels[i].max_br);
      AddCapability(new_cap);
    }
  }
  // add fake H.263p capabilities, need only for H.323
  if(CheckCapability("H.263p{sw}"))
  {
    for(int i = 0; h263_resolutions[i].width != 0; ++i)
    {
      if(PString(h263_resolutions[i].mpiname) == "CIF") // skip default capability
        continue;
      MCUCapability *new_cap = MCUCapability::Create("H.263p{sw}");
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      SetFormatParams(wf, h263_resolutions[i].width, h263_resolutions[i].height);
      AddCapability(new_cap);
    }
  }
  // add fake H.263 capabilities, need only for H.323
  if(CheckCapability("H.263{sw}"))
  {
    for(int i = 0; h263_resolutions[i].width != 0; ++i)
    {
      if(PString(h263_resolutions[i].mpiname) == "CIF") // skip default capability
        continue;
      MCUCapability *new_cap = MCUCapability::Create("H.263{sw}");
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      SetFormatParams(wf, h263_resolutions[i].width, h263_resolutions[i].height);
      AddCapability(new_cap);
    }
  }
  // add fake H.261 capabilities, need only for H.323
  if(CheckCapability("H.261{sw}"))
  {
    for(int i = 0; h263_resolutions[i].width != 0; ++i)
    {
      if(PString(h263_resolutions[i].mpiname) == "CIF") // skip default capability
        continue;
      if(PString(h263_resolutions[i].mpiname) != "QCIF")
        continue;
      MCUCapability *new_cap = MCUCapability::Create("H.261{sw}");
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      SetFormatParams(wf, h263_resolutions[i].width, h263_resolutions[i].height);
      AddCapability(new_cap);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PINDEX MCUH323EndPoint::AddCapabilitiesMCU(PINDEX descriptorNum, PINDEX simultaneous, const char **caps)
{
  PINDEX reply = descriptorNum == P_MAX_INDEX ? P_MAX_INDEX : simultaneous;

  int capNum = 0;
  while(caps[capNum] != NULL)
  {
    PString capName(caps[capNum]);
    OpalMediaFormat mediaFormat(capName);
    if(!mediaFormat.IsValid() && (capName.Right(4) == "{sw}") && capName.GetLength() > 4)
      mediaFormat = OpalMediaFormat(capName.Left(capName.GetLength()-4));
    if(mediaFormat.IsValid())
    {
      // add the capability
      MCUCapability * capability = MCUCapability::Create(capName);
      PINDEX num = capabilities.SetCapability(descriptorNum, simultaneous, capability);
      if(descriptorNum == P_MAX_INDEX)
      {
        reply = num;
        descriptorNum = num;
        simultaneous = P_MAX_INDEX;
      }
      else if (simultaneous == P_MAX_INDEX)
      {
        if(reply == P_MAX_INDEX)
          reply = num;
        simultaneous = num;
      }
    }
    capNum++;
  }
  return reply;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::TranslateTCPAddress(PIPSocket::Address &localAddr, const PIPSocket::Address &remoteAddr)
{
  if (this->behind_masq)
  {
    BYTE byte1=localAddr.Byte1();
    BYTE byte2=localAddr.Byte2();
    const BOOL local_mcu=(
       (byte1==10)                       // LAN class A
     ||((byte1==172)&&((byte2&240)==16)) // LAN class B
     ||((byte1==192)&&(byte2==168))      // LAN class C
     ||((byte1==169)&&(byte2==254))      // APIPA/IPAC/zeroconf (probably LAN)
    );
    if(!local_mcu){
      PTRACE(3, trace_section << "NAT remIP=" << remoteAddr << ", locIP=" << localAddr << ": n/a (src is global)");
      return;
    }

    if(!nat_lag_ip.IsEmpty())
    if(nat_lag_ip.Find(","+remoteAddr.AsString()+",")!=P_MAX_INDEX)
    { PTRACE(3, trace_section << "NAT remIP=" << remoteAddr << ", locIP=" << localAddr << ": ***change to " << *(this->masqAddressPtr) << " (treating as global)");
      localAddr=*(this->masqAddressPtr);
      return;
    }

    byte1=remoteAddr.Byte1();
    byte2=remoteAddr.Byte2();
    if((byte1==10)                       // LAN class A
     ||((byte1==172)&&((byte2&240)==16)) // LAN class B
     ||((byte1==192)&&(byte2==168))      // LAN class C
     ||((byte1==169)&&(byte2==254))      // APIPA/IPAC/zeroconf (probably LAN)
    ) { PTRACE(3, trace_section << "NAT remIP=" << remoteAddr << ", locIP=" << localAddr << ": n/a (dest. is LAN)"); }
    else
    { PTRACE(3, trace_section << "NAT remIP=" << remoteAddr << ", locIP=" << localAddr << ": ***change to " << *(this->masqAddressPtr));
      localAddr=*(this->masqAddressPtr);
    }
  } else
  { PTRACE(3, trace_section << "NAT remIP=" << remoteAddr << ", locIP=" << localAddr << ": n/a (NAT IP not set)"); }
  return;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString JsQuoteScreen(PString s)
{
  PString r="\"";
  for(PINDEX i=0; i<s.GetLength(); i++)
  { BYTE c=(BYTE)s[i];
    if(c>31)
    { if     (c==0x22) r+="\\x22"; // "
      else if(c==0x5c) r+="\\x5c"; // backslash
      else if(c=='<') r+="&lt;";
      else if(c=='>') r+="&gt;";
      else r+=(char)c;
    }
    else
    {
      if(c==9) r+="&nbsp;|&nbsp;"; //tab
      if(c==10) if(r.Right(1)!=" ") r+=" ";
      if(c==13) if(r.Right(1)!=" ") r+=" ";
    }
  }
  r+="\"";
  return r;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetRoomStatusJS()
{
  PString str = "Array(";
  PTime now;

  BOOL firstConference = TRUE;
  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();
  for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it)
  {
    Conference *conference = it.GetObject();
    PStringStream c;

    c << "Array("
        << JsQuoteScreen(conference->GetNumber())                              // c[r][0]: room name
        << "," << conference->GetMemberList().GetCurrentSize()                 // c[r][1]: memberList size
        << "," << conference->GetProfileList().GetCurrentSize()                 // c[r][2]: profileList size
        << "," << PString((now - conference->GetStartTime()).GetMilliSeconds())// c[r][3]: duration
        << ",Array("                                                           // c[r][4]: member descriptors
    ;

    {
      BOOL firstMember = TRUE;
      MCUMemberList & memberList = conference->GetMemberList();
      for(MCUMemberList::shared_iterator it2 = memberList.begin(); it2 != memberList.end(); ++it2)
      {
        ConferenceMember * member = *it2;

        if(!firstMember) c << ",";
        c << "Array("                                                          // c[r][4][m]: member m descriptor
          << (long)member->GetID()                                             // c[r][4][m][0]: member id
          << "," << JsQuoteScreen(member->GetName())                           // c[r][4][m][1]: member name
          << "," << (member->IsVisible() ? "1" : "0")                          // c[r][4][m][2]: is member visible: 1/0
          << "," << PString(member->GetType())                                 // c[r][4][m][3]: 0-NONE, 1-MCU ...
        ;

        PTimeInterval duration;
        PString formatString, audioCodecR, audioCodecT, videoCodecR, videoCodecT, ra;
        int codecCacheMode=-1, cacheUsersNumber=-1;
        MCUH323Connection * conn = NULL;
        DWORD orx=0, otx=0, vorx=0, votx=0, prx=0, ptx=0, vprx=0, vptx=0, plost=0, vplost=0, plostTx=0, vplostTx=0;
        bool isAudioCache = false;
        if(member->GetType() == MEMBER_TYPE_PIPE)
        {
          duration = now - member->GetStartTime();
        }
        else if(member->GetType() == MEMBER_TYPE_RECORDER)
        {
          duration = PTime() - member->GetStartTime();
        }
        else if(member->GetType() == MEMBER_TYPE_STREAM)
        {
          duration = PTime() - member->GetStartTime();
          conn = FindConnectionWithLock(member->GetCallToken());
          if(conn)
          {
            audioCodecT = conn->GetAudioTransmitCodecName();
            MCU_RTP_UDP *sess = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultAudioSessionID);
            if(sess != NULL)
            {
              orx = sess->GetOctetsReceived(); otx = sess->GetOctetsSent();
              prx = sess->GetPacketsReceived(); ptx = sess->GetPacketsSent();
              plost = sess->GetPacketsLost(); plostTx = sess->GetPacketsLostTx();
            }
#if MCU_VIDEO
            videoCodecT = conn->GetVideoTransmitCodecName();
            MCU_RTP_UDP *vSess = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultVideoSessionID);
            if(vSess != NULL)
            {
              vorx=vSess->GetOctetsReceived(); votx=vSess->GetOctetsSent();
              vprx=vSess->GetPacketsReceived(); vptx=vSess->GetPacketsSent();
              vplost = vSess->GetPacketsLost(); vplostTx = vSess->GetPacketsLostTx();
            }

            conn->GetChannelsMutex().Wait();
            if(conn->GetVideoTransmitChannel() != NULL)
            {
              codecCacheMode = conn->GetVideoTransmitChannel()->GetCacheMode();
              formatString = conn->GetVideoTransmitChannel()->GetCacheName();
            }
            conn->GetChannelsMutex().Signal();
#endif
            ra = conn->GetRemoteApplication();
            conn->Unlock();
          }
        }
        else if(member->GetType() == MEMBER_TYPE_CACHE)
        {
          ConferenceCacheMember * cacheMember = dynamic_cast<ConferenceCacheMember *>(member);
          if(cacheMember)
          {
            isAudioCache = cacheMember->IsAudio();
            formatString = cacheMember->GetCacheName();
            cacheUsersNumber = cacheMember->GetCacheUsersNumber();
            codecCacheMode = 1;
          }
          duration = now - member->GetStartTime();
        }
        else // real (visible, external) endpoint
        {
          conn = FindConnectionWithLock(member->GetCallToken());
          if(conn!=NULL)
          {
              duration = now - conn->GetConnectionStartTime();
              audioCodecR = conn->GetAudioReceiveCodecName();
              audioCodecT = conn->GetAudioTransmitCodecName();
              MCU_RTP_UDP *sess = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultAudioSessionID);
              if(sess != NULL)
              {
                orx = sess->GetOctetsReceived(); otx = sess->GetOctetsSent();
                prx = sess->GetPacketsReceived(); ptx = sess->GetPacketsSent();
                plost = sess->GetPacketsLost(); plostTx = sess->GetPacketsLostTx();
              }
#             if MCU_VIDEO
                videoCodecR = conn->GetVideoReceiveCodecName() + "@" + member->GetVideoRxFrameSize();
                videoCodecT = conn->GetVideoTransmitCodecName();
                MCU_RTP_UDP *vSess = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultVideoSessionID);
                if(vSess != NULL)
                {
                  vorx=vSess->GetOctetsReceived(); votx=vSess->GetOctetsSent();
                  vprx=vSess->GetPacketsReceived(); vptx=vSess->GetPacketsSent();
                  vplost = vSess->GetPacketsLost(); vplostTx = vSess->GetPacketsLostTx();
                }

                conn->GetChannelsMutex().Wait();
                if(conn->GetVideoTransmitChannel() != NULL)
                {
                  codecCacheMode = conn->GetVideoTransmitChannel()->GetCacheMode();
                  formatString = conn->GetVideoTransmitChannel()->GetCacheName();
                }
                conn->GetChannelsMutex().Signal();

#             endif
              ra = conn->GetRemoteApplication();
              conn->Unlock();
          }
        }

        c << "," << duration.GetMilliSeconds()                                 // c[r][4][m][4]: member duration
          << "," << orx << "," << otx << "," << vorx << "," << votx            // c[r][4][m][5-8]: orx, otx, vorx, votx
          << "," << JsQuoteScreen(audioCodecR)                                 // c[r][4][m][9]: audio receive codec name
          << "," << JsQuoteScreen(audioCodecT)                                 // c[r][4][m][10]: audio transmit codec name
          << "," << JsQuoteScreen(videoCodecR)                                 // c[r][4][m][11]: video receive codec name
          << "," << JsQuoteScreen(videoCodecT)                                 // c[r][4][m][12]: video transmit codec name
          << "," << codecCacheMode << "," << JsQuoteScreen(formatString)       // c[r][4][m][13,14]: codecCacheMode, formatString
          << "," << member->GetVideoRxFrameRate()                              // c[r][4][m][15]: video rx frame rate
          << "," << member->GetVideoTxFrameRate()                              // c[r][4][m][16]: video tx frame rate
          << "," << cacheUsersNumber                                           // c[r][4][m][17]: cache users number
          << "," << prx << "," << ptx << "," << vprx << "," << vptx            // c[r][4][m][18-21]: prx, ptx, vprx, vptx
          << "," << JsQuoteScreen(ra)                                          // c[r][4][m][22]: remote application name
          << "," << plost << "," << vplost << "," << plostTx << "," << vplostTx// c[r][4][m][23-26]: rx & tx_from_RTCP packets lost (audio, video)
          << "," << isAudioCache                                               // c[r][4][m][27]: audio cache
          << ")";
        firstMember = FALSE;
      }
      c << "))";
    }

    if(!firstConference) str += ",";
    firstConference = FALSE;
    str += c;
  }

  str += ")";
  return str;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetRoomStatusJSStart()
{
  PStringStream html;
  BeginPage(html,"Connections","window.l_connections","window.l_info_connections");
  html
    << "<script>var loadCounter=0;function page_reload(){if(loadCounter<=0) location.href=location.href;document.getElementById('status2').innerHTML=loadCounter;loadCounter--;setTimeout(page_reload, 990);}function status_init(){if(window.status_update_start) setTimeout(status_update_start,500);else{document.getElementById(\"status1\").innerHTML=\"<h1>ERROR: Can not load <font color=red>status.js</font></h1><h2>Page will reload after <span id='status2'>30</span> s</h2>\";loadCounter=30;setTimeout(page_reload, 990);}}</script>"
    << "<div id=\"status1\"></div><script src='status.js'></script>";
  EndPage(html,OpenMCU::Current().GetHtmlCopyright());
  return html;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetRoomStatus(const PString & block)
{
  PString substitution;

  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();
  for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it)
  {
    Conference *conference = it.GetObject();
    // make a copy of the repeating html chunk
    PString insert = block;
    PStringStream members;
    members << 
	"<table class=\"table table-striped table-bordered table-condensed\">"
               "<tr>"
                 "<th>"
                 "&nbsp;Name&nbsp;"
                 "</th><th>"
                 "&nbsp;Duration&nbsp;"
                 "</th><th>"
                 "&nbsp;RTP Channel: Codec&nbsp;"
                 "</th><th>"
                 "&nbsp;Packets&nbsp;"
                 "</th><th>"
                 "&nbsp;Bytes&nbsp;"
                 "</th><th>"
                 "&nbsp;Kbit/s&nbsp;"
                 "</th>"
#if MCU_VIDEO
                 "<th>"
                 "&nbsp;FPS&nbsp;"
                 "</th>"
#endif
                 "</tr>";

    size_t profileListSize = 0;
    PStringArray targets, subses, errors;
    {
      MCUMemberList & memberList = conference->GetMemberList();
      for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
      {
        ConferenceMember * member = *it;

        PString formatString;
        int codecCacheMode=-1, cacheUsersNumber=0;
        BOOL visible=member->IsVisible();
        BOOL cache=(member->GetType() == MEMBER_TYPE_CACHE);
        members << "<tr><td>";
        if(cache) members << "<nobr><b>[Hidden]</b> cache</nobr></td>";
        else
          members
            << (visible? "" : "<b>[Hidden]</b> ")
            << (member->GetType() == MEMBER_TYPE_MCU ? "<b>[MCU]</b> " : "")
            << member->GetName() << "</td>";

        MCUH323Connection * conn = NULL;
        if(!member->GetType() & MEMBER_TYPE_GSYSTEM)
          conn = FindConnectionWithLock(member->GetCallToken());

        PTime now;

        if(conn!=NULL)
        {
#if MCU_VIDEO
          conn->GetChannelsMutex().Wait();
          if(conn->GetVideoTransmitChannel() != NULL)
          {
            codecCacheMode = conn->GetVideoTransmitChannel()->GetCacheMode();
            formatString = conn->GetVideoTransmitChannel()->GetCacheName();
          }
          else
            formatString="NO_CODEC";
          conn->GetChannelsMutex().Signal();
#endif
          PTimeInterval duration = now - conn->GetConnectionStartTime();
          PStringStream d;
          d << duration;
          d=d.Left(d.Find('.'));
          if(d.GetLength()==1) d="00:0"+d;
          else if(d.GetLength()==2) d="00:"+d;
          else if(d.GetLength()==4) d="0"+d;
          members
            << "<td style='text-align:right'>" << d << "</td>"
            << "<td><nobr>"
              << "<b>Audio In: </b>"  << conn->GetAudioReceiveCodecName()
              << "<br /><b>Audio Out: </b>" << conn->GetAudioTransmitCodecName()
#if MCU_VIDEO
              << "<br /><b>Video In: </b>"  << conn->GetVideoReceiveCodecName() << "@" << member->GetVideoRxFrameSize()
              << "<br /><b>"
                << ((codecCacheMode==2)? "<font color=green>":"")
                << "Video Out"
                << ((codecCacheMode==2)? "</font>":"")
                << ": </b>" << conn->GetVideoTransmitCodecName()
#endif
              << "</nobr></td><td style='text-align:right'>";

          DWORD orx=0, otx=0; MCU_RTP_UDP * session = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultAudioSessionID);
#if MCU_VIDEO
          DWORD vorx=0, votx=0; MCU_RTP_UDP * v_session = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultVideoSessionID);
#endif
          if(session!=NULL)
          {
            orx = session->GetOctetsReceived(); otx = session->GetOctetsSent();
            members << session->GetPacketsReceived() << "<br />" << session->GetPacketsSent();
          } else
            members << "-<br />-";
#if MCU_VIDEO
          if(v_session!=NULL)
          {
            vorx = v_session->GetOctetsReceived(); votx = v_session->GetOctetsSent();
            members << "<br />" << v_session->GetPacketsReceived() << "<br />" << v_session->GetPacketsSent();
          } else
            members << "<br />-<br />-";
#endif
          members << "</td><td style='text-align:right'>";
          if(session!=NULL) members << orx << "<br />" << otx; else members << "-<br />-";
#if MCU_VIDEO
          if(v_session!=NULL) members << "<br />" << vorx << "<br />" << votx; else members << "<br />-<br />-";
#endif
          members << "</td><td style='text-align:right'><nobr>";
          if(session!=NULL) members << psprintf("%6.1f",floor(orx * 80.0 / duration.GetMilliSeconds() + 0.5) / 10)
                        << "<br />" << psprintf("%6.1f",floor(otx * 80.0 / duration.GetMilliSeconds() + 0.5) / 10);
          else members << "-<br />-";
#if MCU_VIDEO
          if(v_session!=NULL) members << "<br />" << psprintf("%6.1f",floor(vorx * 80.0 / duration.GetMilliSeconds() + 0.5) / 10)
                                      << "<br />" << psprintf("%6.1f",floor(votx * 80.0 / duration.GetMilliSeconds() + 0.5) / 10);
          else members << "<br />-<br />-";
#endif
          members << "</nobr></td>";

          conn->Unlock();
        }

        if(conn==NULL)
        {
          formatString="NO_CODEC";
          if(cache)
          {
            cache=FALSE;
            ConferenceCacheMember * cacheMember = dynamic_cast<ConferenceCacheMember *>(member);
            if(cacheMember)
            {
              cache = TRUE;
              formatString = cacheMember->GetCacheName();
              cacheUsersNumber = cacheMember->GetCacheUsersNumber();
            }
          }
          PStringStream d;
          d << (now - member->GetStartTime());
          d=d.Left(d.Find('.'));
          if(d.GetLength()==1) d="00:0"+d;
          else if(d.GetLength()==2) d="00:"+d;
          else if(d.GetLength()==4) d="0"+d;
          members
            << "<td style='text-align:right'>" << d << "</td>"
            << "<td>" << (cache? ("<nobr><b>Video Out:  </b>"+formatString+"</nobr>") : "-") << "</td>"
            << "<td>-</td><td>-</td><td>-</td>";

        }

#if MCU_VIDEO
        members << "<td style='text-align:right'>";
        if(visible) members << "<br /><br />";
        if(cache)
        {
          PString target="%%[" + formatString +"]";
          PStringStream subs; subs << psprintf("%4.2f",floor(member->GetVideoTxFrameRate()*100+0.55)/100);
          targets.AppendString(target);
          subses.AppendString(subs);
          members << "<nobr><b><font color=green>" << cacheUsersNumber << " x </font></b>" << subs << "</nobr>";
        }
        else if(visible)
        {
          members << "<nobr>" << psprintf("%4.2f",floor(member->GetVideoRxFrameRate()*100+0.55)/100) << "<br />";
          if(codecCacheMode==2)
          {
            PString t = "%%[" + formatString + "]";
            members << t;
            if(errors.GetStringsIndex(t)==P_MAX_INDEX) errors.AppendString(t);
          }
          else members << psprintf("%4.2f",floor(member->GetVideoTxFrameRate()*100+0.55)/100);
          members << "</nobr>";
        }
        else members << "-";
        members << "</td>";
#endif
        members 
          << "</tr>";
      }

      MCUProfileList & profileList = conference->GetProfileList();
      profileListSize = profileList.GetCurrentSize();
      for(MCUProfileList::shared_iterator it = profileList.begin(); it != profileList.end(); ++it)
      {
        ConferenceMember *member = it->GetMember();
#if MCU_VIDEO
        if(member == NULL) {members << "<tr><td colspan='7'><b>[Offline]</b> <font color='gray'>" << it->GetName() << "</font></td></tr>"; continue; }
#else
        if(member == NULL) {members << "<tr><td colspan='6'><b>[Offline]</b> <font color='gray'>" << it->GetName() << "</font></td></tr>"; continue; }
#endif
      }
    }

    members << "</table>";
    for(PINDEX i=0; i<errors.GetSize(); i++)
    {
      PString target=errors[i], subs;
      PINDEX j = targets.GetStringsIndex(target);
      if(j!=P_MAX_INDEX) subs="<font color='green'>" + subses[j] + "</font>"; else subs="<font color=red>Error</font>";
      members.Replace(target, subs, TRUE, 0);
    }

    SpliceMacro(insert, "RoomName",        conference->GetNumber());
    SpliceMacro(insert, "RoomMemberCount", PString(PString::Unsigned, profileListSize));
    SpliceMacro(insert, "RoomMembers",     members);
    substitution += insert;
  }

  return substitution;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetConferenceOptsJavascript(Conference & c)
{
  PStringStream r; //conf[0]=[videoMixerCount,bfw,bfh):
  PString jsRoom=c.GetNumber(); jsRoom.Replace("&","&amp;",TRUE,0); jsRoom.Replace("\"","&quot;",TRUE,0);
  r << "conf=[[" //l1&l2 open
    << c.GetVideoMixerList().GetCurrentSize()                           // [0][0]  = mixerCount
    << "," << OpenMCU::vmcfg.bfw                                        // [0][1]  = base frame width
    << "," << OpenMCU::vmcfg.bfh                                        // [0][2]  = base frame height
    << ",\"" << jsRoom << "\""                                          // [0][3]  = room name
    << ",'" << c.IsModerated() << "'"                                   // [0][4]  = control
    << ",'" << c.IsMuteUnvisible() << "'"                               // [0][5]  = global mute
    << "," << c.VAlevel << "," << c.VAdelay << "," << c.VAtimeout       // [0][6-8]= vad

    << ",["; // l3 open

  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();
  int number = 0;
  for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it, ++number)
  {
    Conference *conference = it.GetObject();
      jsRoom = conference->GetNumber();
      jsRoom.Replace("&","&amp;",TRUE,0);
      jsRoom.Replace("\"","&quot;",TRUE,0);
      if(number != 0) r << ",";                                                // [0][9][ci][0-2] roomName & memberCount & isModerated
      r << "[\"" << jsRoom << "\"," << conference->GetVisibleMemberCount() << ",\"" << conference->IsModerated() << "\"]";
  }

    r << "]"; // l3 close

  r << "," << OpenMCU::Current().GetScaleFilterType();                      // [0][10] = yuv resizer filter mode

  if(c.conferenceRecorder != NULL && c.conferenceRecorder->IsRunning()) r << ",1"; else r << ",0"; // [0][11] = video recording state (1=recording, 0=NO)
  if(c.lockedTemplate) r << ",1"; else r << ",0";                         // [0][12] = member list locked by template (1=yes, 0=no)

  r << "]"; //l2 close

  MCUVideoMixerList & videoMixerList = c.GetVideoMixerList();
  for(MCUVideoMixerList::shared_iterator it = videoMixerList.begin(); it != videoMixerList.end(); ++it)
  {
    MCUSimpleVideoMixer *mixer = it.GetObject();
    r << "," << GetVideoMixerConfiguration(mixer, it.GetIndex());
  }

  r << "];"; //l1 close
  return r;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetVideoMixerConfiguration(MCUVideoMixer * mixer, int number)
{
  if(mixer == NULL)
    return "[]";
  unsigned n = mixer->GetPositionSet();
  VMPCfgSplitOptions & split=OpenMCU::vmcfg.vmconf[n].splitcfg;
  VMPCfgOptions      * p    =OpenMCU::vmcfg.vmconf[n].vmpcfg;
  PStringStream r;
  r << "[";

  r << "["                                                // a[0]: base parameters:
    << split.mockup_width << "," << split.mockup_height   //   a[0][0-1] = mw * mh
    << "," << n                                           //   a[0][2]   = position set (layout)
    << "," << number                                      //   a[0][3]   = number
    << "],[";

  for(unsigned i=0;i<split.vidnum;i++)
    r << "[" << p[i].posx                                 // a[1]: frame geometry for each position i:
      << "," << p[i].posy                                 // a[1][i][0-1]= posx & posy
      << "," << p[i].width                                // a[1][i][2-3]= width & height
      << "," << p[i].height
      << "," << p[i].border                               // a[1][i][4]  = border
      << "]" << ((i==split.vidnum-1) ? "" : ",");

  r << "]," << mixer->VMPListScanJS()                     // a[2], a[3]: members' ids & types
    << "]";

  return r;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetActiveMemberDataJS(ConferenceMember * member)
{
  if(!member) return "[]";
  PStringStream r;
  r
/* 0*/  << "[1"                                           // [i][ 0] = 1 : ONLINE
/* 1*/  << ",\"" << dec << (long)member->GetID() << "\""  // [i][ 1] = long id
/* 2*/  << ",\"" << member->GetNameHTML() << "\""         // [i][ 2] = name [ip]
/* 3*/  << "," << member->muteMask                        // [i][ 3] = mute
/* 4*/  << "," << member->disableVAD                      // [i][ 4] = disable vad
/* 5*/  << "," << member->chosenVan                       // [i][ 5] = chosen van
/* 6*/  << "," << member->GetAudioLevel()                 // [i][ 6] = audiolevel (peak)
/* 7*/  << "," << member->GetVideoMixerNumber()           // [i][ 7] = number of mixer member receiving
/* 8*/  << ",\"" << member->GetNameID() << "\""           // [i][ 8] = memberName id
/* 9*/  << "," << (unsigned short)member->channelCheck    // [i][ 9] = RTP channels checking bit mask 0000vVaA
/*10*/  << "," << member->kManualGainDB                   // [i][10] = Audio level gain for manual tune, integer: -20..60
/*11*/  << "," << member->kOutputGainDB                   // [i][11] = Output audio gain, integer: -20..60
/*12*/  << "," << GetVideoMixerConfiguration(member->videoMixer, 0) // [i][12] = mixer configuration
        << "]";
  return r;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetMemberListOptsJavascript(Conference & conference)
{
  PStringStream members;
  members << "members=[";
  BOOL firstMember = TRUE;

  MCUProfileList & profileList = conference.GetProfileList();
  for(MCUProfileList::shared_iterator it = profileList.begin(); it != profileList.end(); ++it)
  {
    ConferenceProfile *profile = it.GetObject();
    ConferenceMember *member = profile->GetMember();
    if(member && member->GetType() & MEMBER_TYPE_GSYSTEM)
      continue;
    if(!firstMember)
      members << ",";
    if(member == NULL) // inactive member
    {
      members
/* 0*/  << "[0"
/* 1*/  << ",0"
/* 2*/  << ",\"" << profile->GetNameHTML() << "\""
/* 3*/  << ",0"
/* 4*/  << ",0"
/* 5*/  << ",0"
/* 6*/  << ",0"
/* 7*/  << ",0"
/* 8*/  << ",\"" << profile->GetNameID() << "\""
        << "]";
    }
    else // active member
    {
      members << GetActiveMemberDataJS(member);
    }
    firstMember = FALSE;
  }

  members << "];";
  return members;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int MCUH323EndPoint::SetMemberVideoMixer(Conference & conference, ConferenceMember * member, int newMixerNumber)
{
  if(member->GetType() & MEMBER_TYPE_GSYSTEM)
    return FALSE;

  if(newMixerNumber == (int)member->GetVideoMixerNumber())
    return TRUE;

  MCUSimpleVideoMixer *mixer = conferenceManager.GetVideoMixerWithLock(&conference, newMixerNumber);
  if(mixer == NULL)
    return -1;
  mixer->Unlock();

  MCUH323Connection *conn = FindConnectionWithLock(member->GetCallToken());
  if(conn == NULL)
    return -1;

  conn->GetChannelsMutex().Wait();
  if(conn->GetVideoTransmitChannel() != NULL)
  {
    member->SetVideoMixerNumber(newMixerNumber);
    conn->videoMixerNumber = newMixerNumber;

    MCU_RTPChannel *channel = conn->GetVideoTransmitChannel();
    if(channel->GetCodec() != NULL && channel->GetCacheMode() == 2)
      conn->OpenVideoChannel(TRUE, *((H323VideoCodec *)channel->GetCodec()));
  }
  conn->GetChannelsMutex().Signal();

  conn->Unlock();

  PStringStream cmd;
  cmd << "chmix(" << (long)member << "," << newMixerNumber << ")";
  OpenMCU::Current().HttpWriteCmdRoom(cmd, conference.GetNumber());

  return newMixerNumber;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323EndPoint::OTFControl(const PString room, const PStringToString & data)
{
  PTRACE(6,"WebCtrl\tRoom " << room << ", action " << data("action") << ", value " << data("v") << ", options " << data("o") << ", " << data("o2") << ", " << data("o3"));

  if(!data.Contains("action"))
    return FALSE;
  int action = data("action").AsInteger();

  if(!data.Contains("v"))
    return FALSE;
  PString value = data("v");
  long v = value.AsInteger();

  if(action == OTFC_YUV_FILTER_MODE)
  {
    PString filterName = OpenMCU::Current().SetScaleFilterType(v);
    OpenMCU::Current().HttpWriteEventRoom("filter: "+filterName, room);
    PStringStream cmd;
    cmd << "conf[0][10]=" << v;
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    OpenMCU::Current().HttpWriteCmdRoom("top_panel()",room);
    OpenMCU::Current().HttpWriteCmdRoom("alive()",room);
    return TRUE;
  }

  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();
  MCUConferenceList::shared_iterator cit = conferenceList.Find(room);
  if(cit == conferenceList.end())
    return FALSE;
  Conference *conference = *cit;

  if(action == OTFC_REFRESH_ABOOK)
  {
    conference->RefreshAddressBook();
    return TRUE;
  }
  if(action == OTFC_REFRESH_VIDEO_MIXERS)
  {
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("mixrfr()",room);
    return TRUE;
  }
  if(action == OTFC_VIDEO_RECORDER_START)
  {
    if(conference->StartRecorder())
    {
      return TRUE;
    }
    return FALSE;
  }
  if(action == OTFC_VIDEO_RECORDER_STOP)
  {
    conference->StopRecorder();
    return TRUE;
  }
  if(action == OTFC_TEMPLATE_RECALL)
  {
    OpenMCU::Current().HttpWriteCmdRoom("alive()",room);

    if(conference->IsModerated()=="-")
    {
      conference->SetModerated(TRUE);
      MCUSimpleVideoMixer * mixer = conferenceManager.GetVideoMixerWithLock(conference);
      if(mixer)
      {
        mixer->SetForceScreenSplit(TRUE);
        mixer->Unlock();
      }
      conference->PutChosenVan();
      OpenMCU::Current().HttpWriteEventRoom("<span style='background-color:#bfb'>Operator took the control</span>",room);
      OpenMCU::Current().HttpWriteCmdRoom("r_moder()",room);
    }

    conference->confTpl = conference->ExtractTemplate(value);
    conference->LoadTemplate(conference->confTpl);
    conference->SetLastUsedTemplate(value);
    PStringStream msg;
    msg << GetMemberListOptsJavascript(*conference) << "\n"
        << "p." << GetConferenceOptsJavascript(*conference) << "\n"
        << "p.tl=Array" << conference->GetTemplateList() << "\n"
        << "p.seltpl=\"" << conference->GetSelectedTemplateName() << "\"\n"
        << "p.build_page()";
      OpenMCU::Current().HttpWriteCmdRoom(msg,room);
    return TRUE;
  }
  if(action == OTFC_SAVE_TEMPLATE)
  {
    OpenMCU::Current().HttpWriteCmdRoom("alive()",room);
    PString templateName=value.Trim();
    if(templateName=="") return FALSE;
    if(templateName.Right(1) == "*") templateName=templateName.Left(templateName.GetLength()-1).RightTrim();
    if(templateName=="") return FALSE;
    conference->confTpl = conference->SaveTemplate(templateName);
    conference->TemplateInsertAndRewrite(templateName, conference->confTpl);
    conference->LoadTemplate(conference->confTpl);
    PStringStream msg;
    msg << GetMemberListOptsJavascript(*conference) << "\n"
        << "p." << GetConferenceOptsJavascript(*conference) << "\n"
        << "p.tl=Array" << conference->GetTemplateList() << "\n"
        << "p.seltpl=\"" << conference->GetSelectedTemplateName() << "\"\n"
        << "p.build_page()";
      OpenMCU::Current().HttpWriteCmdRoom(msg,room);
    return TRUE;
  }
  if(action == OTFC_DELETE_TEMPLATE)
  {
    OpenMCU::Current().HttpWriteCmdRoom("alive()",room);
    PString templateName=value.Trim();
    if(templateName=="") return FALSE;
    if(templateName.Right(1) == "*") return FALSE;
    conference->DeleteTemplate(templateName);
    conference->LoadTemplate("");
    conference->SetLastUsedTemplate("");
    PStringStream msg;
    msg << GetMemberListOptsJavascript(*conference) << "\n"
        << "p." << GetConferenceOptsJavascript(*conference) << "\n"
        << "p.tl=Array" << conference->GetTemplateList() << "\n"
        << "p.seltpl=\"" << conference->GetSelectedTemplateName() << "\"\n"
        << "p.build_page()";
      OpenMCU::Current().HttpWriteCmdRoom(msg,room);
    return TRUE;
  }
  if(action == OTFC_TOGGLE_TPL_LOCK)
  {
    PString templateName=value.Trim();
    if((templateName.IsEmpty())||(templateName.Right(1) == "*")) if(!conference->lockedTemplate) return FALSE;
    conference->lockedTemplate = !conference->lockedTemplate;
    if(conference->lockedTemplate) OpenMCU::Current().HttpWriteCmdRoom("tpllck(1)",room);
    else OpenMCU::Current().HttpWriteCmdRoom("tpllck(0)",room);
    return TRUE;
  }
  if(action == OTFC_INVITE)
  {
    Invite(conference->GetNumber(), value);
    return TRUE;
  }
  if(action == OTFC_ADD_AND_INVITE)
  {
    conference->AddMemberToList(value, NULL);
    PString username(value);
    username.Replace("&","&amp;",TRUE,0); username.Replace("\"","&quot;",TRUE,0);
    PString id = MCUURL(username).GetMemberNameId();
    OpenMCU::Current().HttpWriteCmdRoom("addmmbr(0,0,'"+username+"',0,0,0,0,0,'"+id+"',0)", room);
    Invite(conference->GetNumber(), value);
    return TRUE;
  }
  if(action == OTFC_REMOVE_OFFLINE_MEMBER)
  {
    conference->RemoveMemberFromList(value, NULL);
    PStringStream msg;
    msg << GetMemberListOptsJavascript(*conference) << "\n"
        << "p.members_refresh()";
    OpenMCU::Current().HttpWriteCmdRoom(msg,room);
    return TRUE;
  }
  if(action == OTFC_DROP_ALL_ACTIVE_MEMBERS)
  {
    MCUMemberList & memberList = conference->GetMemberList();
    for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
    {
      ConferenceMember * member = *it;
      if(member->GetType() & MEMBER_TYPE_GSYSTEM)
        continue;
      member->Close();
    }
    OpenMCU::Current().HttpWriteEventRoom("Active members dropped by operator",room);
    OpenMCU::Current().HttpWriteCmdRoom("drop_all()",room);
    return TRUE;
  }
  if(action == OTFC_MUTE_ALL || action == OTFC_UNMUTE_ALL)
  {
    MCUMemberList & memberList = conference->GetMemberList();
    for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
    {
      ConferenceMember * member = *it;
      if(member->GetType() & MEMBER_TYPE_GSYSTEM)
        continue;
      if(action == OTFC_MUTE_ALL) member->SetChannelPauses  (1);
      else                        member->UnsetChannelPauses(1);
    }
    return TRUE;
  }
  if(action == OTFC_INVITE_ALL_INACT_MMBRS)
  {
    MCUProfileList & profileList = conference->GetProfileList();
    for(MCUProfileList::shared_iterator it = profileList.begin(); it != profileList.end(); ++it)
    {
      if(it->GetMember() == NULL)
        Invite(room, it->GetName());
    }
    return TRUE;
  }
  if(action == OTFC_REMOVE_ALL_INACT_MMBRS)
  {
    MCUProfileList & profileList = conference->GetProfileList();
    for(MCUProfileList::shared_iterator it = profileList.begin(); it != profileList.end(); ++it)
    {
      ConferenceProfile *profile = it.GetObject();
      if(profile->GetMember() == NULL)
      {
        if(profileList.Erase(it))
          delete profile;
      }
    }
    OpenMCU::Current().HttpWriteEventRoom("Offline members removed by operator",room);
    OpenMCU::Current().HttpWriteCmdRoom("remove_all()",room);
    return TRUE;
  }
  if(action == OTFC_TAKE_CONTROL)
  {
    if(conference->IsModerated()=="-")
    {
      conference->SetModerated(TRUE);
      MCUSimpleVideoMixer * mixer = conferenceManager.GetVideoMixerWithLock(conference);
      if(mixer)
      {
        mixer->SetForceScreenSplit(TRUE);
        mixer->Unlock();
      }
      conference->PutChosenVan();
      OpenMCU::Current().HttpWriteEventRoom("<span style='background-color:#bfb'>Operator took the control</span>",room);
      OpenMCU::Current().HttpWriteCmdRoom("r_moder()",room);
    }
    return TRUE;
  }
  if(action == OTFC_DECONTROL)
  {
    if(conference->IsModerated()=="+")
    {
      conference->SetModerated(FALSE);
      MCUSimpleVideoMixer * mixer = conferenceManager.GetVideoMixerWithLock(conference);
      if(mixer)
      {
        mixer->SetForceScreenSplit(FALSE);
        mixer->Unlock();
      }
      UnmoderateConference(*conference);
      OpenMCU::Current().HttpWriteEventRoom("<span style='background-color:#acf'>Operator resigned</span>",room);
      OpenMCU::Current().HttpWriteCmdRoom("r_unmoder()",room);
      OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
      OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    }
    return TRUE;
  }
  if(action == OTFC_ADD_VIDEO_MIXER)
  {
    if(conference->GetVideoMixerList().GetCurrentSize() == 0)
      return FALSE;
    if(conference->IsModerated()=="+")
    {
      unsigned n = conferenceManager.AddVideoMixer(conference);
      PStringStream msg; msg << "Video mixer " << n << " added";
      OpenMCU::Current().HttpWriteEventRoom(msg,room);
      OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
      OpenMCU::Current().HttpWriteCmdRoom("mmw=-1;p.build_page()",room);
      return TRUE;
    }
    return FALSE;
  }
  if(action == OTFC_DELETE_VIDEO_MIXER)
  {
    if(conference->GetVideoMixerList().GetCurrentSize() == 0)
      return FALSE;
    if(conference->IsModerated()=="+")
    {
      unsigned n_old = conference->GetVideoMixerList().GetCurrentSize();
      unsigned n = conferenceManager.DeleteVideoMixer(conference, v);
      if(n_old != n)
      {
        PStringStream msg; msg << "Video mixer " << v << " removed";
        OpenMCU::Current().HttpWriteEventRoom(msg,room);
        OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
        OpenMCU::Current().HttpWriteCmdRoom("mmw=-1;p.build_page()",room);
        return TRUE;
      }
    }
    return FALSE;
  }
  if(action == OTFC_SET_VIDEO_MIXER_LAYOUT)
  {
    long option = data("o").AsInteger();
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, option);
    if(mixer == NULL)
      return FALSE;
    mixer->MyChangeLayout(v);
    mixer->Unlock();
    conference->PutChosenVan();
    conference->FreezeVideo(NULL);
    if(conference->GetVideoMixerList().GetCurrentSize() != 0)
      OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    else
      OpenMCU::Current().HttpWriteCmdRoom(GetMemberListOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_REMOVE_VMP)
  {
    unsigned pos = data("o").AsInteger();
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->MyRemoveVideoSource(pos,TRUE);
    mixer->Unlock();
    conference->FreezeVideo(NULL);
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_ARRANGE_VMP)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    MCUMemberList & memberList = conference->GetMemberList();
    for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
    {
      ConferenceMember * member = *it;
      if(member->IsVisible())
      {
        if(mixer->AddVideoSourceToLayout(member->GetID(), *(member)))
           member->SetFreezeVideo(FALSE);
        else
          break;
      }
    }
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_CLEAR)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->MyRemoveAllVideoSource();
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_SHUFFLE_VMP)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->Shuffle();
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_SCROLL_LEFT)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->Scroll(TRUE);
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_SCROLL_RIGHT)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->Scroll(FALSE);
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MIXER_REVERT)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    mixer->Revert();
    mixer->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_GLOBAL_MUTE)
  {
    if(data("v") == "true") v = 1;
    if(data("v") == "false") v = 0;
    conference->SetMuteUnvisible((BOOL)v);
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_SET_VAD_VALUES)
  {
    conference->VAlevel   = (unsigned short int) v;
    conference->VAdelay   = (unsigned short int) (data("o").AsInteger());
    conference->VAtimeout = (unsigned short int) (data("o2").AsInteger());
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_MOVE_VMP)
  {
    if(!data.Contains("o2"))
      return FALSE;
    long option2 = data("o2").AsInteger();
    MCUSimpleVideoMixer *mixer1 = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer1 == NULL)
      return FALSE;
    MCUSimpleVideoMixer *mixer2 = conferenceManager.FindVideoMixerWithLock(conference, option2);
    if(mixer2 == NULL)
    {
      mixer1->Unlock();
      return FALSE;
    }
    int pos1 = data("o").AsInteger(); int pos2 = data("o3").AsInteger();
    if(mixer1 == mixer2)
    {
      mixer1->Exchange(pos1,pos2);
    } else {
      ConferenceMemberId id = mixer1->GetHonestId(pos1); if(((long)id<100)&&((long)id>=0)) id=NULL;
      ConferenceMemberId id2 = mixer2->GetHonestId(pos2); if(((long)id2<100)&&((long)id2>=0)) id2=NULL;
      ConferenceMember *member1 = conferenceManager.FindMemberWithLock(conference, (long)id);
      ConferenceMember *member2 = conferenceManager.FindMemberWithLock(conference, (long)id2);
      mixer2->PositionSetup(pos2, 1, member1);
      mixer1->PositionSetup(pos1, 1, member2);
      if(member1)
        member1->Unlock();
      if(member2)
        member2->Unlock();
    }
    mixer1->Unlock();
    mixer2->Unlock();
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if(action == OTFC_VAD_CLICK)
  {
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, v);
    if(mixer == NULL)
      return FALSE;
    unsigned pos = data("o").AsInteger();
    int type = data("o2").AsInteger();
    if((type<1)||(type>3)) type=2;
    long id = (long)mixer->GetHonestId(pos);
//    if((type==1)&&(id>=0)&&(id<100)) //static but no member
    if((id>=0)&&(id<100)) //just no member (and we want to add him for some reason)
    {
      BOOL setup = FALSE;
      MCUMemberList & memberList = conference->GetMemberList();
      for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
      {
        ConferenceMember * member = *it;
        if(member->IsVisible())
        {
          if(  (mixer->VMPListFindVMP(member->GetID()) == NULL)  &&  ((type==1)||(!member->disableVAD))  )
          {
            mixer->PositionSetup(pos, type, member);
            member->SetFreezeVideo(FALSE);
            setup = TRUE;
            break;
          }
        }
      }
      if(setup == FALSE)
        mixer->PositionSetup(pos,type,NULL);
    }
//    else if((id>=0)&&(id<100))
//      mixer->PositionSetup(pos,type,NULL);
    else
      mixer->SetPositionType(pos,type);
    mixer->Unlock();
    conference->FreezeVideo(NULL);
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }

  MCUMemberList & memberList = conference->GetMemberList();
  MCUMemberList::shared_iterator mit = memberList.Find(v);
  if(mit == memberList.end())
    return FALSE;
  ConferenceMember * member = *mit;
  PStringStream cmd;

  if(action == OTFC_SET_VMP_STATIC )
  {
    if(member->GetType() & MEMBER_TYPE_GSYSTEM)
      return FALSE;
    long n = data("o").AsInteger();
    MCUSimpleVideoMixer *mixer = conferenceManager.FindVideoMixerWithLock(conference, n);
    if(mixer == NULL)
      return FALSE;
    int pos = data("o2").AsInteger();
    mixer->PositionSetup(pos, 1, member);
    mixer->Unlock();
    member->SetFreezeVideo(FALSE);
    OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
    return TRUE;
  }
  if( action == OTFC_AUDIO_GAIN_LEVEL_SET )
  {
    int n=data("o").AsInteger();
    if(n<0) n=0;
    if(n>80) n=80;
    member->kManualGainDB=n-20;
    member->kManualGain=(float)pow(10.0,((float)member->kManualGainDB)/20.0);
    cmd << "setagl(" << v << "," << member->kManualGainDB << ")";
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    return TRUE;
  }
  if( action == OTFC_OUTPUT_GAIN_SET )
  {
    int n=data("o").AsInteger();
    if(n<0) n=0;
    if(n>80) n=80;
    member->kOutputGainDB=n-20;
    member->kOutputGain=(float)pow(10.0,((float)member->kOutputGainDB)/20.0);
    cmd << "setogl(" << v << "," << member->kOutputGainDB << ")";
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    return TRUE;
  }
  if(action == OTFC_MUTE)
  {
    if(!member->GetType() & MEMBER_TYPE_GSYSTEM)
      member->SetChannelPauses(data("o").AsInteger());
    return TRUE;
  }
  if(action == OTFC_UNMUTE)
  {
    if(!member->GetType() & MEMBER_TYPE_GSYSTEM)
      member->UnsetChannelPauses(data("o").AsInteger());
    return TRUE;
  }
  if(action == OTFC_REMOVE_FROM_VIDEOMIXERS)
  {
    if(conference->IsModerated()=="+")
    {
      MCUVideoMixerList & videoMixerList = conference->GetVideoMixerList();
      if(videoMixerList.GetCurrentSize() != 0)
      {
        for(MCUVideoMixerList::shared_iterator it = videoMixerList.begin(); it != videoMixerList.end(); ++it)
        {
          MCUSimpleVideoMixer *mixer = it.GetObject();
          int oldPos = mixer->GetPositionNum(member->GetID());
          if(oldPos != -1)
            mixer->MyRemoveVideoSource(oldPos, TRUE);
        }
      }
      else // classic MCU mode
      {
        MCUMemberList & memberList = conference->GetMemberList();
        for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
        {
          ConferenceMember * member = *it;
          MCUVideoMixer * mixer = member->videoMixer;
          int oldPos = mixer->GetPositionNum(member->GetID());
          if(oldPos != -1) mixer->MyRemoveVideoSource(oldPos, TRUE);
        }
      }
      if(!member->GetType() & MEMBER_TYPE_GSYSTEM)
        member->SetFreezeVideo(TRUE);
      OpenMCU::Current().HttpWriteCmdRoom(GetConferenceOptsJavascript(*conference),room);
      OpenMCU::Current().HttpWriteCmdRoom("build_page()",room);
      return TRUE;
    }
    return TRUE;
  }
  if(action == OTFC_DROP_MEMBER )
  {
    member->Close();
    return TRUE;
  }
  if(action == OTFC_VAD_NORMAL)
  {
    member->disableVAD=FALSE;
    member->chosenVan=FALSE;
    conference->PutChosenVan();
    cmd << "ivad(" << v << ",0)";
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    return TRUE;
  }
  if(action == OTFC_VAD_CHOSEN_VAN)
  {
    member->disableVAD=FALSE;
    member->chosenVan=TRUE;
    conference->PutChosenVan();
    conference->FreezeVideo(member->GetID());
    cmd << "ivad(" << v << ",1)";
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    return TRUE;
  }
  if(action == OTFC_VAD_DISABLE_VAD)
  {
    member->disableVAD=TRUE;
    member->chosenVan=FALSE;
    conference->PutChosenVan();
#if 1 // DISABLING VAD WILL CAUSE MEMBER REMOVAL FROM VAD POSITIONS
    {
      ConferenceMemberId id = member->GetID();
      MCUVideoMixerList & videoMixerList = conference->GetVideoMixerList();
      if(videoMixerList.GetCurrentSize() != 0)
      {
        for(MCUVideoMixerList::shared_iterator it = videoMixerList.begin(); it != videoMixerList.end(); ++it)
        {
          MCUSimpleVideoMixer *mixer = it.GetObject();
          int type = mixer->GetPositionType(id);
          if(type == 2 || type == 3)
            mixer->MyRemoveVideoSourceById(id, FALSE);
        }
      }
      else
      {
        MCUMemberList & memberList = conference->GetMemberList();
        for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
        {
          ConferenceMember * member = *it;
          MCUVideoMixer * mixer = member->videoMixer;
          int type = mixer->GetPositionType(id);
          if(type<2 || type>3) continue; //-1:not found, 1:static, 2&3:VAD
          mixer->MyRemoveVideoSourceById(id, FALSE);
        }
      }
      conference->FreezeVideo(id);
    }
#endif
    cmd << "ivad(" << v << ",2)";
    OpenMCU::Current().HttpWriteCmdRoom(cmd,room);
    return TRUE;
  }
  if(action == OTFC_SET_MEMBER_VIDEO_MIXER)
  {
    int option = data("o").AsInteger();
    int newMixerNumber = SetMemberVideoMixer(*conference, member, option);
    if(newMixerNumber == -1)
      return FALSE;
    return TRUE;
  }

  return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetRoomList(const PString & block)
{
  PString substitution;
  PString insert = block;
  PStringStream members;
  members << "<input name='room' id='room' type=hidden>";

  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();
  for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it)
  {
    Conference *conference = it.GetObject();

    PString roomNumber = conference->GetNumber();
    // make a copy of the repeating html chunk
    members << "<span class=\"btn btn-large";
    if(conference->IsModerated()=="+") members << " btn-success";
    else members << " btn-primary";
    members << "\" onclick='document.getElementById(\"room\").value=\"" << roomNumber << "\";document.forms[0].submit();'>"
            << roomNumber << " " << conference->IsModerated() << " " << conference->GetVisibleMemberCount()
	    << "</span>";
  }

  members << "";
  SpliceMacro(insert, "List", members);
  substitution += insert;
  return substitution;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::RoomCtrlPage(const PString room)
{
 PStringStream page;

 BeginPage(page,"Room Control","window.l_control","window.l_info_control");

 page << "<script src=\"control.js\"></script>";

 page << "<div id='cb1' name='cb1' class='input-append'>&nbsp;</div>"
   << "<div style='text-align:center'>"
     << "<div id='cb2' name='cb2' style='margin-left:auto;margin-right:auto;width:100px;height:100px;background-color:#ddd'>"
       << "<div id='logging0' style='position:relative;top:0px;left:-50px;width:0px;height:0px'>"
         << "<div id='logging1' style='position:absolute;width:50px;height:50px'>"
           << "<iframe style='background-color:#eef;border:1px solid #55c;padding:0px;margin:0px' id='loggingframe' name='loggingframe' src='Comm?room=" << room << "' width=50 height=50>"
             << "Your browser does not support IFRAME tag :("
           << "</iframe>"
         << "</div>"
       << "</div>"
       << "<div id='cb3' name='cb3' style='position:relative;top:0px;left:0px;width:0px;height:0px'>"
         << "&nbsp;"
       << "</div>"
     << "</div>"
   << "</div>"
 ;
 EndPage(page,OpenMCU::Current().GetHtmlCopyright());
 return page;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323EndPoint::UnmoderateConference(Conference & conference)
{
  conference.SetMuteUnvisible(FALSE);

  MCUSimpleVideoMixer * mixer = conferenceManager.GetVideoMixerWithLock(&conference);
  if(mixer == NULL)
    return;
  mixer->MyRemoveAllVideoSource();
  mixer->Unlock();

  MCUMemberList & memberList = conference.GetMemberList();
  for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
  {
    ConferenceMember * member = *it;
    if(member->GetType() != MEMBER_TYPE_CACHE)
      SetMemberVideoMixer(conference, member, 0);
    if(!member->IsVisible())
      continue;
    if(mixer->AddVideoSource(member->GetID(), *member))
      member->SetFreezeVideo(FALSE);
    else
      member->SetFreezeVideo(TRUE);
  }

  MCUVideoMixerList & videoMixerList = conference.GetVideoMixerList();
  for(MCUVideoMixerList::shared_iterator it = videoMixerList.begin(); it != videoMixerList.end(); ++it)
  {
    MCUSimpleVideoMixer * _mixer = *it;
    if(_mixer == mixer)
      continue;
    if(videoMixerList.Erase(it))
      delete _mixer;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::SetRoomParams(const PStringToString & data)
{
  PString room = data("room");

  // "On-the-Fly" Control via XMLHTTPRequest:
  if(data.Contains("otfc"))
    return (OTFControl(room, data) ? "OK" : "FAIL");

  if(data.Contains("refresh")) // JavaScript data refreshing
  {
    PTRACE(6,"WebCtrl\tJS refresh");
    Conference *conference = conferenceManager.FindConferenceWithLock(room);
    if(conference == NULL)
      return "";

    PString data = GetMemberListOptsJavascript(*conference);
    conference->Unlock();
    return data;
  }

  OpenMCU::Current().HttpWriteEventRoom("MCU Operator connected",room);
  PTRACE(6,"WebCtrl\tOperator connected");

  Conference *conference = conferenceManager.FindConferenceWithLock(room);
  if(conference == NULL)
    return "OpenMCU-ru: Bad room";
  conference->Unlock();

  return RoomCtrlPage(room);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::GetMonitorText()
{
  PStringStream output;

  MCUConferenceList & conferenceList = conferenceManager.GetConferenceList();

  output << "Room Count: " << conferenceList.GetCurrentSize() << "\n"
         << "Max Room Count: " << conferenceManager.GetMaxConferenceCount() << "\n";

  PINDEX confNum = 0;

  for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it)
  {
    Conference *conference = it.GetObject();

    output << "\n[Conference "     << ++confNum << "]\n"
           << "Title: "            << conference->GetNumber() << "\n"
           << "ID: "               << conference->GetID() << "\n"
           << "Duration: "         << (PTime() - conference->GetStartTime()) << "\n"
           << "Member Count: "     << conference->GetMemberList().GetCurrentSize() << "\n"
           << "Max Member Count: " << conference->GetMaxMemberCount() << "\n";

    PINDEX num = 0;
    MCUMemberList & memberList = conference->GetMemberList();
    for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
    {
      ConferenceMember * member = *it;

      output << "[Member " << ++num << "]\n";
      PStringStream hdr;
      hdr << "  ";

      output << hdr << "Name: " << member->GetName() << "\n"
             << hdr << "Outgoing video mixer: " << member->GetVideoMixerNumber() << "\n"
             << hdr << "Duration: " << (PTime() - member->GetStartTime()) << "\n"
             << member->GetMonitorInfo(hdr);
      if(!member->GetType() & MEMBER_TYPE_GSYSTEM)
      {
        output << hdr << "callToken: " << member->GetCallToken() << "\n";
      }
      if(member->GetType() == MEMBER_TYPE_CACHE)
      {
        ConferencePipeMember * pipeMember = dynamic_cast<ConferencePipeMember *>(member);
        if(pipeMember!=NULL)
        {
          output << hdr << "Format: " << pipeMember->GetFormat() << "\n";
          output << hdr << "IsVisible: " << pipeMember->IsVisible() << "\n";
        }
      }
      if(member->GetType() == MEMBER_TYPE_PIPE)
      {
        ConferenceCacheMember * cacheMember = dynamic_cast<ConferenceCacheMember *>(member);
        if(cacheMember!=NULL)
        {
          output << hdr << "Format: " << cacheMember->GetMediaFormat() << "\n";
          output << hdr << "IsVisible: " << cacheMember->IsVisible() << "\n";
          output << hdr << "Status: " << (cacheMember->GetStatus()?"Awake":"Sleeping") << "\n";
//#ifndef _WIN32
//            if(fileMember->codec) output << hdr << "EncoderSeqN: " << dec << fileMember->codec->GetEncoderSeqN() << "\n";
//#endif
        }
      }
      if(member->videoMixer!=NULL)
      {
        output << hdr << "Video Mixer ID: " << member->videoMixer << "\n";
        int n=member->videoMixer->GetPositionSet();
        output << hdr << "Video Mixer Layout ID: " << OpenMCU::vmcfg.vmconf[n].splitcfg.Id << "\n"
          << hdr << "Video Mixer Layout capacity: " << dec << OpenMCU::vmcfg.vmconf[n].splitcfg.vidnum << hex << "\n";
        MCUVideoMixer::VideoMixPosition *r=member->videoMixer->vmpList->next;
        while(r!=NULL)
        {
          output << hdr << "[Position " << r->n << "]\n"
                 << hdr << "  Conference Member Id: " << r->id << "\n"
                 << hdr << "  Position type: " << r->type << "\n"
                 << hdr << "  Position status: " << r->status << "\n";
          r=r->next;
        }
      }
    }

    MCUVideoMixerList & videoMixerList = conference->GetVideoMixerList();
    int number = 0;
    for(MCUVideoMixerList::shared_iterator it = videoMixerList.begin(); it != videoMixerList.end(); ++it, ++number)
    {
      MCUSimpleVideoMixer *mixer = it.GetObject();
      output << "[Mixer " << number << "]\n";
      int n = mixer->GetPositionSet();
      output << "  Layout ID: "       << OpenMCU::vmcfg.vmconf[n].splitcfg.Id << "\n"
             << "  Layout capacity: " << OpenMCU::vmcfg.vmconf[n].splitcfg.vidnum << "\n"
             << mixer->GetFrameStoreMonitorList();

      MCUVideoMixer::VideoMixPosition *r = mixer->vmpList->next;
      while(r != NULL)
      {
        output << "  [Position " << r->n << "]\n"
               << "    Conference Member Id: " << r->id << "\n"
               << "    Position type: " << r->type << "\n"
               << "    Position status: " << r->status << "\n";
        r=r->next;
      }
    }
  }

  return output;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::Invite(PString room, PString memberName)
{
  MCUURL url(memberName);
  if(url.GetUserName() == "" && url.GetHostName() == "")
    return "";

  PString callToken;
  PString address = url.GetUrl();
  PStringStream msg;
  msg << "Inviting: " << address << " ";

  if(url.GetScheme() == "h323")
  {
    BOOL allowLoopbackCalls = MCUConfig("Parameters").GetBoolean(AllowLoopbackCallsKey, FALSE);
    if(!allowLoopbackCalls && HasListener(url.GetHostName(), url.GetPort()))
    {
      msg << "failed, Loopback call rejected";
      goto end;
    }
  }
  else if(url.GetScheme() == "sip")
  {
    MCUSipEndPoint * sep = OpenMCU::Current().GetSipEndpoint();
    if(sep->HasListener(address))
    {
      msg << "failed, Loopback call rejected";
      goto end;
    }
  }
  else if(url.GetScheme() == "rtsp")
  {
    MCURtspServer *rtsp = OpenMCU::Current().GetRtspServer();
    if(rtsp->HasListener(url.GetHostName(), url.GetPort()))
    {
      msg << "failed, Loopback call rejected";
      goto end;
    }
  }

  {
    Registrar *registrar = OpenMCU::Current().GetRegistrar();
    registrar->MakeCall(room, address, callToken);
    if(callToken == "")
      msg << ", failed";
  }

  end:
    PTRACE(1, trace_section << msg);
    OpenMCU::Current().HttpWriteEventRoom(msg, room);
    return callToken;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323EndPoint::IncomingConferenceRequest(H323Connection & connection, 
                                                  const H323SignalPDU & setupPDU,
                                                  unsigned & videoMixerNumber)
{
  const H225_Setup_UUIE & setup = setupPDU.m_h323_uu_pdu.m_h323_message_body;

  /*
   Here is the algorithm used for joining a conference.

   - If the conference goal is e_invite, then refuse the call

   - if the conference ID matches the ID of an existing conference, then join that conference

   - If there is no destination address, join the default conference

   - If there is a destination address, try and match the destination address
     to a conference number. If there is a match, join to that conference

   - If the destination address does not match any conference, create a new conference
  */

  // get the conference ID from the incoming call
  OpalGloballyUniqueID conferenceID = setup.m_conferenceID;

  PString roomToJoin;

  // check the conference ID
  if (conferenceManager.HasConference(conferenceID, roomToJoin)) {
    PTRACE(3, trace_section << "Using conference ID to join existing room " << roomToJoin);
    videoMixerNumber = 0;
    return roomToJoin;
  }

  // look at destination addresses
  PINDEX i;
  for (i = 0; (i < setup.m_destinationAddress.GetSize()); i++) {
    roomToJoin = H323GetAliasAddressString(setup.m_destinationAddress[i]);
    // allow calls to room/VIDEOMIXER@number
    { PINDEX slashPos = roomToJoin.Find('/');
      if(slashPos != P_MAX_INDEX)
      { PString mixerStr = roomToJoin.Right(roomToJoin.GetLength()-slashPos-1);
        PINDEX mixerStrLen = mixerStr.GetLength();
        if(mixerStrLen > 0 && mixerStrLen < 3)
        { PINDEX mixer = mixerStr.AsInteger();
          if(mixer > 0 && mixer < 100)
          { roomToJoin=roomToJoin.Left(slashPos);
            videoMixerNumber = mixer;
    } } } }
    if (conferenceManager.HasConference(roomToJoin)) {
      PTRACE(3, trace_section << "Joining room specified by destination address " << roomToJoin);
      return roomToJoin;
    }
  }

  // look at Q931 called party number
//  if (roomToJoin.IsEmpty() && !setupPDU.GetQ931().GetCalledPartyNumber(roomToJoin) && roomToJoin.IsEmpty()) {
  if (roomToJoin.IsEmpty())
  if (setupPDU.GetQ931().GetCalledPartyNumber(roomToJoin))
  if (!roomToJoin.IsEmpty())
  {
    // allow calls to room/VIDEOMIXER@number
    { PINDEX slashPos = roomToJoin.Find('/');
      if(slashPos != P_MAX_INDEX)
      { PString mixerStr = roomToJoin.Right(roomToJoin.GetLength()-slashPos-1);
        PINDEX mixerStrLen = mixerStr.GetLength();
        if(mixerStrLen > 0 && mixerStrLen < 3)
        { PINDEX mixer = mixerStr.AsInteger();
          if(mixer > 0 && mixer < 100)
          { roomToJoin=roomToJoin.Left(slashPos);
            videoMixerNumber = mixer;
    } } } }
    if (conferenceManager.HasConference(roomToJoin)) {
      PTRACE(3, trace_section << "Joining room specified by Q.931 called party " << roomToJoin);
      return roomToJoin;
    }
  }

  // if there is a room to create, then join this call to that conference
  if (roomToJoin.IsEmpty()) 
    roomToJoin = OpenMCU::Current().GetDefaultRoomName();

  if (!roomToJoin.IsEmpty()) {
    PTRACE(3, trace_section << "Joining default room " << roomToJoin);
    videoMixerNumber = 0;
    return roomToJoin;
  }

  PTRACE(3, trace_section << "Refusing call because no room specified, and no default room");
  return PString::Empty();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

NotifyH245Thread::NotifyH245Thread(Conference & conference, BOOL _join, ConferenceMember * _memberToIgnore)
  : PThread(10000, AutoDeleteThread), join(_join), memberToIgnore(_memberToIgnore)
{
  mcuNumber  = conference.GetID();
  terminalIdToSend = (long)memberToIgnore->GetID();

  // create list of connections to notify
  MCUMemberList & memberList = conference.GetMemberList();
  for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
  {
    ConferenceMember * member = *it;
    if(member->GetType() & MEMBER_TYPE_GSYSTEM)
      continue;
    if(member != memberToIgnore)
      tokens += member->GetCallToken();
  }

  Resume();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void NotifyH245Thread::Main()
{
  MCUH323EndPoint & ep = OpenMCU::Current().GetEndpoint();

  // send H.245 message on each connection in turn
  PINDEX i;
  for (i = 0; i < tokens.GetSize(); ++i) {
    MCUH323Connection * h323Conn = ep.FindConnectionWithLock(tokens[i]);

      H323ControlPDU pdu;
      H245_ConferenceIndication & ind = pdu.Build(H245_IndicationMessage::e_conferenceIndication);
      ind.SetTag(join ? H245_ConferenceIndication::e_terminalJoinedConference : H245_ConferenceIndication::e_terminalLeftConference);
      H245_TerminalLabel & terminalId = ind;
      terminalId.m_mcuNumber      = mcuNumber;
      terminalId.m_terminalNumber = terminalIdToSend;

      h323Conn->WriteControlPDU(pdu);

      h323Conn->Unlock();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323Connection::MCUH323Connection(MCUH323EndPoint & _ep, unsigned callReference, void * userData)
  : H323Connection(_ep, callReference), ep(_ep), isMCU(FALSE)
{
  conference       = NULL;
  conferenceMember = NULL;
  welcomeState     = NotStartedYet;
  connectionType   = CONNECTION_TYPE_H323;
  clearing         = false;

#if MCU_VIDEO
  videoMixerNumber = 0;
#endif

  if(userData != NULL)
  {
    requestedRoom = *(PString *)userData;
    PINDEX slashPos = requestedRoom.Find("/");
    if(slashPos!=P_MAX_INDEX)
    {
#if MCU_VIDEO
      videoMixerNumber=requestedRoom.Mid(slashPos+1,P_MAX_INDEX).Trim().AsInteger();
#endif
      requestedRoom=requestedRoom.Left(slashPos);
    }
    delete (PString *)userData;
  }

  if(requestedRoom == "")
    requestedRoom = OpenMCU::Current().GetDefaultRoomName();

  localPartyName = requestedRoom;
  localDisplayName = OpenMCU::Current().GetServerId();
  if(!ep.IsRegisteredWithGatekeeper())
  {
    localAliasNames.RemoveAll();
    localAliasNames.AppendString(OpenMCU::Current().GetName());
    localAliasNames.AppendString(requestedRoom);
  }

  vfuSendTime = PTime(0);
  vfuBeginTime = PTime(0);
  vfuInterval = 0;
  vfuLimit = 0;
  vfuCount = 0;
  vfuTotalCount = 0;

  rtpInputTimeout = DefaultRTPInputTimeout;
  rtpInputLostInterval = 0;
  rtpInputBytes = 0;

#if MCU_VIDEO
  videoGrabber = NULL;
  videoDisplay = NULL;
#endif

  audioReceiveChannel = NULL;
  videoReceiveChannel = NULL;
  audioTransmitChannel = NULL;
  videoTransmitChannel = NULL;

  audioReceiveCodecName = audioTransmitCodecName = "none";
  videoReceiveCodecName = videoTransmitCodecName = "none";

}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUH323Connection::~MCUH323Connection()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::AttachSignalChannel(const PString & token, H323Transport * channel, BOOL answeringCall)
{
  if(answeringCall)
    direction = DIRECTION_INBOUND;
  else
    direction = DIRECTION_OUTBOUND;

  callToken = token;
  trace_section = "H323 Connection "+callToken+": ";
  OnCreated();

  H323Connection::AttachSignalChannel(token, channel, answeringCall);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnCreated()
{
  ep.OnConnectionCreated(this);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnEstablished()
{
  H323Connection::OnEstablished();

  if(requestedRoom == "")
    requestedRoom = OpenMCU::Current().GetDefaultRoomName();

  JoinConference(requestedRoom);

  if(!conference || !conferenceMember || !conferenceMember->IsJoined())
  {
    if(connectionType == CONNECTION_TYPE_H323)
      ClearCall();
  }

  if(conference && conferenceMember && conferenceMember->IsJoined())
  {
    Registrar *registrar = OpenMCU::Current().GetRegistrar();
    registrar->ConnectionEstablished(callToken);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

class TplCleanCheckThread : public PThread
{
  public:
    TplCleanCheckThread(Conference * _c, const PString & _n, const PString & _a)
      : PThread(10000, AutoDeleteThread), c(_c), n(_n), a(_a)
    {
      Resume();
    }
    void Main()
    {
      PTRACE(2,"TplCleanCheckThread\tThread started, c=" << c << ", n=" << n << ", a=" << a);
      MCUTime::Sleep(6000); // previous connection may still be actvie
      if(c!=NULL)
      {
        PTRACE(1,"TplCleanCheckThread\tFind conference " << c);
        if(OpenMCU::Current().GetConferenceManager()->FindConferenceWithLock(c))
        {
          c->OnConnectionClean(n, a);
          c->Unlock();
        }
      }
    }
  protected:
    Conference * c;
    PString n, a;
};

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetCallEndReason(CallEndReason reason, PSyncPoint * sync)
{
  if(connectionType == CONNECTION_TYPE_H323)
  {
    H323Connection::SetCallEndReason(reason, sync);
  }
  else
  {
    PTRACE(3, trace_section << "Call end reason for " << callToken << " set to " << reason);
    callEndReason = reason;
    callEndTime = PTime();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::ClearCall(H323Connection::CallEndReason reason)
{
  return ep.ClearCall(callToken, reason);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::CleanUpOnCallEnd()
{
  PTRACE(2, trace_section << "CleanUpOnCallEnd");

  if(conference && !conference->stopping)
  {
    PTRACE(4, trace_section << "Starting new thread which will check the connection later in template.cxx");
    new TplCleanCheckThread(conference, remotePartyName, remotePartyAddress);
  }

  H323Connection::CleanUpOnCallEnd();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnCleared()
{
  PTRACE(2, trace_section << "OnCleared");

  ep.OnConnectionCleared(*this, callToken);

  LogCall();
  LeaveConference();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetRequestedRoom()
{
  if(direction == DIRECTION_INBOUND)
    requestedRoom = GetEndpointParam(RoomNameKey, requestedRoom);

  Registrar *registrar = OpenMCU::Current().GetRegistrar();
  registrar->SetRequestedRoom(callToken, requestedRoom);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::LeaveConference()
{
  PTRACE(1, trace_section << "Leave conference: " << requestedRoom);

  PWaitAndSignal m(connMutex);

  if(conferenceMember)
    delete conferenceMember;

  conference = NULL;
  conferenceMember = NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::JoinConference(const PString & roomToJoin)
{
  PTRACE(1, trace_section << "Join conference: " << roomToJoin << " remotePartyName: " << remotePartyName);

  PWaitAndSignal m(connMutex);

  if(conference != NULL || conferenceMember != NULL)
    return;

  if(roomToJoin.IsEmpty())
    return;

  // create or join the conference
  ConferenceManager & manager = ((MCUH323EndPoint &)ep).GetConferenceManager();
  conference = manager.MakeConferenceWithLock(roomToJoin);
  if(!conference)
    return;

  conferenceIdentifier = conference->GetGUID();

  // crete member connection
  conferenceMember = new MCUConnection_ConferenceMember(conference, ep, GetCallToken(), isMCU);
  conference->Unlock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

RTP_Session * MCUH323Connection::UseSession(unsigned sessionID,
                                         const H245_TransportAddress & taddr,
			                 H323Channel::Directions dir,
                                         RTP_QOS * rtpqos)
{
  // We only support unicast IP at this time.
  if (taddr.GetTag() != H245_TransportAddress::e_unicastAddress) {
    return NULL;
  }

  const H245_UnicastAddress & uaddr = taddr;
  if (uaddr.GetTag() != H245_UnicastAddress::e_iPAddress
#if P_HAS_IPV6
        && uaddr.GetTag() != H245_UnicastAddress::e_iP6Address
#endif
     ) {
    return NULL;
  }

  RTP_Session * session = (RTP_Session *)rtpSessions.UseSession(sessionID);
  if (session != NULL) {
    ((MCUH323_RTP_UDP *) session)->Reopen(dir == H323Channel::IsReceiver);
    return session;
  }

  MCUH323_RTP_UDP * udp_session = new MCUH323_RTP_UDP(
#ifdef H323_RTP_AGGREGATE
                  useRTPAggregation ? ep.GetRTPAggregator() : NULL, 
#endif
                  sessionID, remoteIsNAT);

  udp_session->SetUserData(new H323_RTP_UDP(*this, *udp_session, rtpqos));
  rtpSessions.AddSession(udp_session);
  return udp_session;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

H323Channel * MCUH323Connection::CreateRealTimeLogicalChannel(const H323Capability & capability,
                                                           H323Channel::Directions dir,
                                                           unsigned sessionID,
							   const H245_H2250LogicalChannelParameters * param,
                                                           RTP_QOS * rtpqos)
{
  RTP_Session * session = NULL;

  if (param != NULL)
    session = UseSession(param->m_sessionID, param->m_mediaControlChannel, dir, rtpqos);
  else {
    // Make a fake transmprt address from the connection so gets initialised with
    // the transport type (IP, IPX, multicast etc).
    H245_TransportAddress addr;
    GetControlChannel().SetUpTransportPDU(addr, H323Transport::UseLocalTSAP);
    session = UseSession(sessionID, addr, dir, rtpqos);
  }

  if (session == NULL)
    return NULL;

  MCUH323_RTPChannel *channel = new MCUH323_RTPChannel(*this, capability, dir, *session);
  return channel;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetupCacheConnection(PString & format, Conference * conf, ConferenceMember * memb)
{
 remotePartyName = format;
 conference = conf;
 conferenceIdentifier = conference->GetGUID();
 conferenceMember = memb;
 requestedRoom = conference->GetNumber();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SelectDefaultLogicalChannel(unsigned sessionID)
{
  if(FindChannel(sessionID, FALSE))
    return;

  MCUH323EndPoint & ep = OpenMCU::Current().GetEndpoint();
  if(ep.tvCaps != NULL && sessionID == RTP_Session::DefaultVideoSessionID)
  {
    H323Capability * remoteCapability = SelectRemoteCapability(remoteCapabilities, sessionID, ep.tvCaps);
    if(remoteCapability == NULL)
      return;
    PTRACE(2, trace_section << "OpenLogicalChannel " << *remoteCapability);
    OpenLogicalChannel(*remoteCapability, sessionID, H323Channel::IsTransmitter);
    return;
  }
  if(ep.tsCaps != NULL && sessionID == RTP_Session::DefaultAudioSessionID)
  {
    H323Capability * remoteCapability = SelectRemoteCapability(remoteCapabilities, sessionID, ep.tsCaps);
    if(remoteCapability == NULL)
      return;
    PTRACE(2, trace_section << "OpenLogicalChannel " << *remoteCapability);
    OpenLogicalChannel(*remoteCapability, sessionID, H323Channel::IsTransmitter);
    return;
  }

  const H323Capabilities & capabilities = ep.GetCapabilities();
  for(PINDEX i = 0; i < capabilities.GetSize(); i++)
  {
    H323Capability & capability = capabilities[i];
    if(capability.GetDefaultSessionID() == sessionID)
    {
      H323Capability * remoteCapability = remoteCapabilities.FindCapability(capability);
      if(remoteCapability != NULL)
      {
        PTRACE(3, trace_section << "Selecting " << *remoteCapability);
        MergeCapabilities(sessionID, capability, remoteCapability);
        if(OpenLogicalChannel(*remoteCapability, sessionID, H323Channel::IsTransmitter))
          break;
        PTRACE(2, trace_section << "OnSelectLogicalChannels, OpenLogicalChannel failed: " << *remoteCapability);
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

H323Capability * MCUH323Connection::SelectRemoteCapability(H323Capabilities & capabilities, unsigned sessionID, char **capsName)
{
  if(capsName == NULL)
    return NULL;
  for(int i = 0; capsName[i] != NULL; i++)
  {
    PString capName(capsName[i]);
    H323Capability * capability = capabilities.FindCapability(capName);
    if(capability != NULL)
     return capability;
  }
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnSetLocalCapabilities()
{
  PTRACE(1, trace_section << "OnSetLocalCapabilities");
  PString audio_cap = GetEndpointParam("Audio codec(receive)");
  if(audio_cap.Left(5) == "G.711" && audio_cap.Right(4) == "{sw}") { audio_cap.Replace("{sw}","",TRUE,0); }
  PString video_cap = GetEndpointParam("Video codec(receive)", false);
  PString video_res = GetEndpointParam("Video resolution(receive)", false);
  if(video_cap == "")
  {
    video_cap = GetEndpointParam("Video codec(receive)");
    video_res = GetEndpointParam("Video resolution(receive)");
  }
  unsigned width = video_res.Tokenise("x")[0].AsInteger();
  unsigned height = video_res.Tokenise("x")[1].AsInteger();
  unsigned bandwidth_to = GetEndpointParam(BandwidthToKey, "0").AsInteger();

  for(PINDEX i = 0; i < localCapabilities.GetSize(); )
  {
    PString capname = localCapabilities[i].GetFormatName();
    if(localCapabilities[i].GetMainType() == MCUCapability::e_Audio && audio_cap != "")
    {
      if(capname != audio_cap)
      {
        localCapabilities.Remove(&localCapabilities[i]);
        continue;
      }
      // ITU‑T Rec. G.722.1 (05/2005)
      if(audio_cap.Find("G.722.1-") == 0)
      {
        OpalMediaFormat & wf = localCapabilities[i].GetWritableMediaFormat();
        int bitrate = wf.GetOptionInteger(OPTION_MAX_BIT_RATE);
        wf.SetOptionInteger(OPTION_MAX_BIT_RATE, bitrate * 100);
      }
    }
    else if(localCapabilities[i].GetMainType() == MCUCapability::e_Video && video_cap != "")
    {
      if(capname != video_cap)
      {
        localCapabilities.Remove(&localCapabilities[i]);
        continue;
      }
      else if(width != 0 && height != 0)
      {
        if(GetPluginName(capname) == "H.261" || GetPluginName(capname) == "H.263" || GetPluginName(capname) == "H.263p")
        {
          PString mpiname;
          GetParamsH263(mpiname, width, height);
          const OpalMediaFormat & mf = localCapabilities[i].GetMediaFormat();
          unsigned cap_mpi = mf.GetOptionInteger(mpiname+" MPI");
          if(cap_mpi == 0)
          { localCapabilities.Remove(&localCapabilities[i]); continue; }
        }
        else if(GetPluginName(capname) == "H.264")
        {
          unsigned level = 0, level_h241 = 0, max_fs = 0;
          max_fs = GetVideoMacroBlocks(width, height);
          GetParamsH264(level, level_h241, max_fs);
          const OpalMediaFormat & mf = localCapabilities[i].GetMediaFormat();
          unsigned cap_level_h241 = mf.GetOptionInteger("Generic Parameter 42");
          if(level_h241 != cap_level_h241)
          { localCapabilities.Remove(&localCapabilities[i]); continue; }
        }
        else if(GetPluginName(capname) == "VP8")
        {
          const OpalMediaFormat & mf = localCapabilities[i].GetMediaFormat();
          unsigned cap_width = mf.GetOptionInteger("Generic Parameter 1");
          unsigned cap_height = mf.GetOptionInteger("Generic Parameter 2");
          if(width != cap_width && height != cap_height)
          { localCapabilities.Remove(&localCapabilities[i]); continue; }
        }
      }
      // set video group
      localCapabilities.SetCapability(0, MCUCapability::e_Video, &localCapabilities[i]);
    }
    if(localCapabilities[i].GetMainType() == MCUCapability::e_Video && bandwidth_to != 0)
    {
      OpalMediaFormat & wf = localCapabilities[i].GetWritableMediaFormat();
      wf.SetOptionInteger(OPTION_MAX_BIT_RATE, bandwidth_to*1000);
    }
    i++;
  }
  //cout << "OnSetLocalCapabilities\n" << localCapabilities << "\n";
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnReceivedCapabilitySet(const H323Capabilities & remoteCaps, const H245_MultiplexCapability * muxCap, H245_TerminalCapabilitySetReject & rejectPDU)
{
  PString audio_cap = GetEndpointParam("Audio codec(transmit)");
  PString video_cap = GetEndpointParam("Video codec(transmit)", false);
  PString video_res = GetEndpointParam("Video resolution(transmit)", false);
  if(video_cap == "")
  {
    video_cap = GetEndpointParam("Video codec(transmit)");
    video_res = GetEndpointParam("Video resolution(transmit)");
  }
  unsigned width = video_res.Tokenise("x")[0].AsInteger();
  unsigned height = video_res.Tokenise("x")[1].AsInteger();
  unsigned frame_rate = GetEndpointParam(FrameRateFromKey, "0").AsInteger();
  unsigned bandwidth = GetEndpointParam(BandwidthFromKey, "0").AsInteger();

  BOOL custom_audio_codec = FALSE;
  BOOL custom_video_codec = FALSE;

  H323Capabilities _remoteCaps;
  for(PINDEX i = 0; i < remoteCaps.GetSize(); i++)
  {
    PString capname = remoteCaps[i].GetFormatName();
    if(remoteCaps[i].GetMainType() == MCUCapability::e_Audio)
    {
      // для аудио всегда создавать новый audio_cap
      if(audio_cap == "")
        _remoteCaps.Copy(remoteCaps[i]);
      else if(audio_cap == capname)
        custom_audio_codec = TRUE;
    }
    else if(remoteCaps[i].GetMainType() == MCUCapability::e_Video)
    {
      if(bandwidth == 0)
      {
        bandwidth = remoteCaps[i].GetMediaFormat().GetOptionInteger(OPTION_MAX_BIT_RATE);
        bandwidth = bandwidth/1000;
      }

      if(video_cap == "")
      {
        OpalMediaFormat & wf = remoteCaps[i].GetWritableMediaFormat();
        // обязательно применить заданные frame_rate,bandwidth
        SetFormatParams(wf, 0, 0, frame_rate, bandwidth);
        _remoteCaps.Copy(remoteCaps[i]);
      }
      else if(GetPluginName(video_cap) == GetPluginName(capname))
      {
        // создать новый video_cap только если задано разрешение,
        // иначе только применить заданные frame_rate,bandwidth
        if(width == 0 && height == 0)
        {
          OpalMediaFormat & wf = remoteCaps[i].GetWritableMediaFormat();
          SetFormatParams(wf, 0, 0, frame_rate, bandwidth);
          _remoteCaps.Copy(remoteCaps[i]);
        }
        else
        {
          custom_video_codec = TRUE;
        }
      }

    }
    else
    {
      _remoteCaps.Copy(remoteCaps[i]);
    }
  }

  // Отправить openLogicalChannel для заданного кодека,
  // даже если его нет в полученном capabilitySet
  if(video_cap != "" && custom_video_codec == FALSE && width != 0 && height != 0)
  {
    custom_video_codec = TRUE;
    for(PINDEX i = 0; i < _remoteCaps.GetSize(); i++)
    {
      PString capname = remoteCaps[i].GetFormatName();
      if(GetPluginName(video_cap) == GetPluginName(capname))
      {
        custom_video_codec = FALSE;
        break;
      }
    }
  }

  // create custom audio capability
  if(custom_audio_codec)
  {
    MCUCapability *new_cap = MCUCapability::Create(audio_cap);
    if(new_cap)
    {
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      if(audio_cap.Find("G.722.1C-") == 0)
      {
        // ITU‑T Rec. G.722.1 (05/2005)
        unsigned supportedExtendedModes = wf.GetOptionInteger("Generic Parameter 2");
        if(supportedExtendedModes == 64)
          wf.SetOptionInteger("Generic Parameter 2", 64); // 24K
        else if(supportedExtendedModes == 96)
          wf.SetOptionInteger("Generic Parameter 2", 32); // 32K
        else if(supportedExtendedModes == 112)
          wf.SetOptionInteger("Generic Parameter 2", 16); // 48K
      }
      _remoteCaps.Add(new_cap);
    }
  }
  // create custom video capability
  if(custom_video_codec)
  {
    MCUCapability *new_cap = MCUCapability::Create(video_cap);
    if(new_cap)
    {
      OpalMediaFormat & wf = new_cap->GetWritableMediaFormat();
      SetFormatParams(wf, width, height, frame_rate, bandwidth);
      _remoteCaps.Add(new_cap);
    }
  }
  //cout << "OnReceivedCapabilitySet\n" << remoteCaps << "\n";
  //cout << "OnReceivedCapabilitySet\n" << _remoteCaps << "\n";

  return H323Connection::OnReceivedCapabilitySet(_remoteCaps, muxCap, rejectPDU);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnH245Request(const H323ControlPDU & pdu)
{
  const H245_RequestMessage & request = pdu;
  if(request.GetTag() == H245_RequestMessage::e_terminalCapabilitySet)
  {
    H323Capabilities copy = localCapabilities;
    localCapabilities = ep.GetCapabilities();
    BOOL ret = H323Connection::OnH245Request(pdu);
    localCapabilities = copy;
    return ret;
  }
  return H323Connection::OnH245Request(pdu);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnSendCapabilitySet(H245_TerminalCapabilitySet & pdu)
{
  H323Connection::OnSendCapabilitySet(pdu);
  //cout << "OnSendCapabilitySet\n" << pdu << "\n";
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::StartControlNegotiations(BOOL renegotiate)
{
  return H323Connection::StartControlNegotiations(renegotiate);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnReceivedSignalSetup(const H323SignalPDU & setupPDU)
{
  const H225_Setup_UUIE & setup = setupPDU.m_h323_uu_pdu.m_h323_message_body;
  isMCU = setup.m_sourceInfo.m_mc;

  BOOL ret = H323Connection::OnReceivedSignalSetup(setupPDU);
  // set endpoint name
  SetRemoteName(setupPDU);
  // override requested room
  SetRequestedRoom();
  return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnReceivedCallProceeding(const H323SignalPDU & proceedingPDU)
{
  const H225_CallProceeding_UUIE & proceeding = proceedingPDU.m_h323_uu_pdu.m_h323_message_body;
  isMCU = proceeding.m_destinationInfo.m_mc;

  BOOL ret = H323Connection::OnReceivedCallProceeding(proceedingPDU);
  SetRemoteName(proceedingPDU);
  return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnReceivedSignalConnect(const H323SignalPDU & pdu)
{
  BOOL ret = H323Connection::OnReceivedSignalConnect(pdu);
  // set endpoint name
  SetRemoteName(pdu);
  // override requested room
  SetRequestedRoom();
  return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

H323Connection::AnswerCallResponse MCUH323Connection::OnAnswerCall(const PString & /*caller*/, const H323SignalPDU & setupPDU, H323SignalPDU & /*connectPDU*/)
{
  requestedRoom = ep.IncomingConferenceRequest(*this, setupPDU, videoMixerNumber);

  // remove prefix, maybe bug on the terminal
  // RealPresence "url_ID "h323:room101""
  requestedRoom.Replace("h323:","",TRUE,0);

  if(requestedRoom.IsEmpty())
    return AnswerCallDenied;

  SetRemoteName(setupPDU);
  Registrar *registrar = OpenMCU::Current().GetRegistrar();
  return registrar->OnReceivedH323Invite(this);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::CheckVFU()
{
  PString delay = GetEndpointParam(ReceivedVFUDelayKey, "");
  if(delay == "" || delay == DisableKey)
    return TRUE;
  if(delay == "0/0")
    return FALSE;

  vfuLimit = delay.Tokenise("/")[0].AsInteger();
  vfuInterval = delay.Tokenise("/")[1].AsInteger();
  if(vfuLimit == 0 || vfuInterval == 0)
    return TRUE;

  PTime now;
  vfuTotalCount++;
  if(now < vfuBeginTime + PTimeInterval(vfuInterval*1000))
  {
    vfuCount++;
  } else {
    if(vfuCount > vfuLimit)
    {
      // only show warning if the number of received VFU requests(vfuCount) for a certain interval(vfuInterval) more than vfuLimit
      PString username = MCUURL(memberName).GetUrl();
      PString event = "too many VFU from \""+username+"\", limit "+PString(vfuLimit)+"/"+PString(vfuInterval)+", received "+PString(vfuCount);
      PTRACE(6, trace_section << "RTP " << event);
      OpenMCU::Current().HttpWriteEventRoom("<font color=red>"+event+"</font>", requestedRoom);
    }
    vfuBeginTime = now;
    vfuCount = 1;
  }
  // skip requests
  if(vfuCount > vfuLimit)
    return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnH245_MiscellaneousCommand(const H245_MiscellaneousCommand & pdu)
{
  if(pdu.m_type.GetTag() == H245_MiscellaneousCommand_type::e_videoFastUpdatePicture)
  {
    if(!CheckVFU())
      return TRUE;

    PWaitAndSignal m(channelsMutex);
    if(videoTransmitChannel && videoTransmitChannel->GetCacheMode() != 0)
    {
      videoTransmitChannel->OnFastUpdatePicture();
      return TRUE;
    }
  }
  return H323Connection::OnH245_MiscellaneousCommand(pdu);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SendLogicalChannelMiscCommand(H323Channel & channel, unsigned command)
{
  if(connectionType != CONNECTION_TYPE_H323)
    return;

  if(command == H245_MiscellaneousCommand_type::e_videoFastUpdatePicture)
  {
    PTime now;
    if(now < vfuSendTime + PTimeInterval(1000))
      return;
    vfuSendTime = now;
  }

  H323Connection::SendLogicalChannelMiscCommand(channel, command);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SendLogicalChannelMiscIndication(H323Channel & channel, unsigned commandIdentifier)
{
  if(connectionType != CONNECTION_TYPE_H323)
    return;

  H323ControlPDU pdu;
  H245_IndicationMessage & indication = pdu.Build(H245_IndicationMessage::e_miscellaneousIndication);
  H245_MiscellaneousIndication & miscIndication = indication;
  miscIndication.m_logicalChannelNumber = (unsigned)channel.GetNumber();
  miscIndication.m_type.SetTag(commandIdentifier);
  WriteControlPDU(pdu);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323Connection::GetEndpointParam(PString param, PString defaultValue, bool asterisk)
{
  PString value = GetEndpointParam(param, asterisk);
  if(value == "")
    return defaultValue;
  return value;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323Connection::GetEndpointParam(PString param, bool asterisk)
{
  PString url = remotePartyAddress;
  if(connectionType == CONNECTION_TYPE_H323)
  {
    PINDEX pos = url.Find("ip$");
    if(pos != P_MAX_INDEX) url=url.Mid(pos+3);
    url = GetRemoteNumber()+"@"+url;
  }
  return GetSectionParamFromUrl(param, url, asterisk);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetEndpointDefaultVideoParams(H323VideoCodec & codec)
{
  if(GetRemotePartyAddress() == "")
    return;

  OpalMediaFormat & mf = codec.GetWritableMediaFormat();

  mf.SetOptionInteger(OPTION_ENCODER_QUALITY, DefaultVideoQuality);

  PStringList keys = MCUConfig("Video").GetKeys();
  for(PINDEX i = 0; i < keys.GetSize(); i++)
  {
    if(keys[i].Tokenise(" ")[0] == videoTransmitCodecName.Tokenise("-")[0])
    {
      PINDEX pos = keys[i].Find(" ");
      if(pos == P_MAX_INDEX)
        continue;
      PString option = keys[i].Right(keys[i].GetSize()-pos-2);
      int value = MCUConfig("Video").GetInteger(keys[i], 0);
      if(option == OPTION_MAX_BIT_RATE)
      {
        value = value*1000;
        if(value == 0 || value > mf.GetOptionInteger(option))
          continue;
      }
      mf.SetOptionInteger(option, value);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OpenAudioChannel(BOOL isEncoding, unsigned /* bufferSize */, H323AudioCodec & codec)
{
  PWaitAndSignal m(channelsMutex);

  const OpalMediaFormat & mf = codec.GetMediaFormat();
  unsigned sampleRate = mf.GetTimeUnits() * 1000;
  unsigned channels = 1;
  if(isEncoding)
    channels = mf.GetOptionInteger(OPTION_ENCODER_CHANNELS, 1);
  else
    channels = mf.GetOptionInteger(OPTION_DECODER_CHANNELS, 1);

  codec.SetSilenceDetectionMode(H323AudioCodec::NoSilenceDetection);

  if(isEncoding)
  {
    audioTransmitChannel = ((MCUFramedAudioCodec &)codec).GetLogicalChannel();
    audioTransmitCodecName = mf + "@" + PString(sampleRate) + "/" +PString(channels);

    // check cache mode
    BOOL enableCache = FALSE;
    if(conferenceMember && conferenceMember->GetType() == MEMBER_TYPE_STREAM)
      enableCache = TRUE;
    if(conferenceMember && conferenceMember->GetType() == MEMBER_TYPE_CACHE)
      enableCache = FALSE;

    // setup cache
    if(enableCache)
    {
      // update format string
      audioTransmitCodecName = audioTransmitCodecName + "_" + requestedRoom;
      if(!OpenAudioCache(requestedRoom, mf, audioTransmitCodecName))
        return FALSE;
      audioTransmitChannel->SetCacheName(audioTransmitCodecName);
      audioTransmitChannel->SetCacheMode(2);
    }

    codec.AttachChannel(new OutgoingAudio(*this, sampleRate, channels), TRUE);

  } else {
    audioReceiveChannel = ((MCUFramedAudioCodec &)codec).GetLogicalChannel();
    audioReceiveCodecName = codec.GetMediaFormat() + "@" + PString(sampleRate) + "/" +PString(channels);

    //if(GetEndpointParam(AudioDeJitterKey, EnableKey) == DisableKey)
    //  audioReceiveChannel->SetAudioJitterEnable(false);

    codec.AttachChannel(new IncomingAudio(*this, sampleRate, channels), TRUE);
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

#if MCU_VIDEO
BOOL MCUH323Connection::OpenVideoChannel(BOOL isEncoding, H323VideoCodec & codec)
{
  PWaitAndSignal m(channelsMutex);

  // get member video mixer
  if(conferenceMember && conferenceMember->IsVisible())
    videoMixerNumber = conferenceMember->GetVideoMixerNumber();

  if(isEncoding)
  {
    videoTransmitChannel = ((MCUVideoCodec &)codec).GetLogicalChannel();
    videoTransmitCodecName = codec.GetMediaFormat();

    // cache mode
    // forceScreenSplit не изменяется в созданной конференции
    BOOL forceScreenSplit = TRUE;
    if(conference)
      forceScreenSplit = conference->GetForceScreenSplit();
    else
      forceScreenSplit = GetConferenceParam(requestedRoom, ForceSplitVideoKey, TRUE);
    int cacheMode = 0;
    if(forceScreenSplit)
    {
      // Для всех по умолчанию 2
      cacheMode = 2;
      // Настройки терминала
      if(GetEndpointParam(VideoCacheKey, "Enable") == "Disable")
        cacheMode = 0;
      // Режим кэширования после подключения не изменяется
      if(videoTransmitChannel && videoTransmitChannel->GetCacheMode() >= 0)
        cacheMode = videoTransmitChannel->GetCacheMode();
      // Для потока кэша всегда 1
      if(conferenceMember && conferenceMember->GetType() == MEMBER_TYPE_CACHE)
        cacheMode = 1;
      // Не понятно что они будут видеть в конференции без forceScreenSplit
      if(conferenceMember && conferenceMember->GetType() == MEMBER_TYPE_STREAM)
        cacheMode = 2;
      // Не работает с кэшем, требует периодический опорный кадр
      if(remoteApplication.Find("PCS-") != P_MAX_INDEX && videoTransmitCodecName.Find("H.264") != P_MAX_INDEX)
      {
        cacheMode = 0;
        videoTransmitChannel->SetIntraRefreshPeriod(125);
      }
    }

    // set params from video config page
    if(cacheMode == 0 || cacheMode == 2)
      SetEndpointDefaultVideoParams(codec);

    // get frame rate from codec
    const OpalMediaFormat & mf = codec.GetMediaFormat();
    unsigned frameRate;
    if(mf.GetOptionInteger(OPTION_FRAME_TIME) != 0)
      frameRate = 90000/mf.GetOptionInteger(OPTION_FRAME_TIME);
    else if(mf.GetOptionInteger(OPTION_FRAME_RATE) != 0)
      frameRate = mf.GetOptionInteger(OPTION_FRAME_RATE);
    else
      frameRate = ep.GetVideoFrameRate();
    codec.SetTargetFrameTimeMs(1000/frameRate); // ???

    // update format string
    videoTransmitCodecName = mf + "@" + PString(codec.GetWidth())
                             + "x" + PString(codec.GetHeight())
                             + ":" + PString(mf.GetOptionInteger(OPTION_MAX_BIT_RATE))
                             + "x" + PString(frameRate);

    // videoGrabber
    // Нужен в режиме без кэша или для потока кэша
    if(cacheMode == 0 || cacheMode == 1)
    {
      PVideoChannel * channel = new PVideoChannel;
      if(videoGrabber)
        channel->CloseVideoReader();

      videoGrabber = new MCUPVideoInputDevice(*this);
      if(videoGrabber == NULL)
      {
        PTRACE(3, trace_section << "Cannot create MCU video input driver");
        return FALSE;
      }

      if(!InitGrabber(videoGrabber, codec.GetWidth(), codec.GetHeight(), frameRate))
      {
        delete videoGrabber;
        videoGrabber = NULL;
        return FALSE;
      }

      videoGrabber->Start();
      channel->AttachVideoReader(videoGrabber);
      if(!codec.AttachChannel(channel,TRUE))
        return FALSE;
    }

    // Режим с кэшированием
    if(cacheMode == 2)
    {
      videoTransmitCodecName = videoTransmitCodecName + "_" + requestedRoom + "/" + (PString)videoMixerNumber;
      if(!OpenVideoCache(requestedRoom, codec.GetMediaFormat(), videoTransmitCodecName))
        return FALSE;
      videoTransmitChannel->SetCacheName(videoTransmitCodecName);
      videoTransmitChannel->SetCacheMode(2);
    }

    if(conferenceMember)
      conferenceMember->ChannelBrowserStateUpdate(8,TRUE);

  } else {

    videoReceiveChannel = ((MCUVideoCodec &)codec).GetLogicalChannel();
    videoReceiveCodecName = codec.GetMediaFormat();

    if(conference && conference->IsModerated() == "+")
      conference->FreezeVideo(this);

    videoDisplay = new MCUPVideoOutputDevice(*this);

    if (!videoDisplay->Open("")) {
      delete videoDisplay;
      return FALSE;
    }

    videoDisplay->SetFrameSize(codec.GetWidth(), codec.GetHeight()); // needed to enable resize
    videoDisplay->SetColourFormatConverter("YUV420P");

    PVideoChannel * channel = new PVideoChannel;
    channel->AttachVideoPlayer(videoDisplay);
    if (!codec.AttachChannel(channel,TRUE))
      return FALSE;
  }

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnStartLogicalChannel(H323Channel & channel)
{
  if(connectionType == CONNECTION_TYPE_H323)
    return H323Connection::OnStartLogicalChannel(channel);

  return ep.OnStartLogicalChannel(*this, channel);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnClosedLogicalChannel(const H323Channel & channel)
{
  PWaitAndSignal m(channelsMutex);

  if(&channel == audioReceiveChannel)
  {
    audioReceiveChannel = NULL;
    audioReceiveCodecName = "none";
  }
  else if(&channel == audioTransmitChannel)
  {
    audioTransmitChannel = NULL;
    audioTransmitCodecName = "none";
  }
  else if(&channel == videoReceiveChannel)
  {
    videoReceiveChannel = NULL;
    videoReceiveCodecName = "none";
  }
  else if(&channel == videoTransmitChannel)
  {
    videoTransmitChannel = NULL;
    videoTransmitCodecName = "none";
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::RestartGrabber()
{
  videoGrabber->Restart();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::InitGrabber(PVideoInputDevice * grabber, int newFrameWidth, int newFrameHeight, int newFrameRate)
{
  PTRACE(4, trace_section << "Video grabber set to " << newFrameWidth << "x" << newFrameHeight);

  //if (!(pfdColourFormat.IsEmpty()))
  //  grabber->SetPreferredColourFormat(pfdColourFormat);

  if (!grabber->Open("", FALSE)) {
    PTRACE(3, trace_section << "Failed to open the video input device");
    return FALSE;
  }

  //if (!grabber->SetChannel(ep.GetVideoPlayMode())) {
  //  PTRACE(3, "Failed to set channel to " << ep.GetVideoPlayMode());
  //  return FALSE;
  //}

  //if (!grabber->SetVideoFormat(
  //    ep.GetVideoIsPal() ? PVideoDevice::PAL : PVideoDevice::NTSC)) {
  //  PTRACE(3, "Failed to set format to " << (ep.GetVideoIsPal() ? "PAL" : "NTSC"));
  //  return FALSE;
  //}

  if (!grabber->SetColourFormatConverter("YUV420P") ) {
    PTRACE(3, trace_section << "Failed to set format to yuv420p");
    return FALSE;
  }


  if (newFrameRate != 0) {
    if (!grabber->SetFrameRate(newFrameRate)) {
      PTRACE(3, trace_section << "Failed to set framerate to " << newFrameRate);
      return FALSE;
    }
  }

  if (!grabber->SetFrameSizeConverter(newFrameWidth,newFrameHeight,FALSE)) {
    PTRACE(3, trace_section << "Failed to set frame size to " << newFrameWidth << "x" << newFrameHeight);
    return FALSE;
  }

  return TRUE;
}

#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnUserInputString(const PString & str)
{
  PWaitAndSignal m(connMutex);

  if (conferenceMember == NULL)
    return;

  PString signalTypes = "1234567890*#ABCD";
  if(str.GetLength() == 1 && signalTypes.Find(str) != P_MAX_INDEX)
  {
    dtmfBuffer += str;
    if(str != "#") return;
  } else {
    dtmfBuffer = str;
  }

  cout << "Receive DTMF command: " << dtmfBuffer << "\n";
  PString dtmfTmp = dtmfBuffer;
  dtmfTmp.Replace("#","",TRUE,0);
  ConferenceMember *codeConferenceMember;
  PString code, codeAction, codeRoom, codeRoomName, codePos;
  PStringStream codeMsg;
  PStringArray codeArray = dtmfTmp.Tokenise("*");
  if(codeArray.GetSize() == 2)
  {
    code = codeArray[1];
  //} else if(codeArray.GetSize() == 3) {
    //codePos = codeArray[1];
    //code = codeArray[2];
  } else if(codeArray.GetSize() == 4) {
    codeRoom = codeArray[1];
    codePos = codeArray[2];
    code = codeArray[3];
  }
  if(code != "" && MCUConfig("Control Codes").HasKey(code))
  {
    PString name, text;
    PStringArray params = MCUConfig("Control Codes").GetString(code).Tokenise(",");
    if(params.GetSize() >= 2)
    {
      codeAction = params[0];
      if(params[1] != "") text = params[1];
      else text = params[0];
    }
    if(connectionType == CONNECTION_TYPE_SIP)
      name = GetRemotePartyName()+" ["+GetRemotePartyAddress()+"]";
    else
      name = GetRemotePartyName();
    codeMsg << "<font color=blue><b>" << name << "</b>: " << text;

    if(codeRoom == "")
    {
      codeConferenceMember = conferenceMember;
    } else {
      codeConferenceMember = NULL;
      codeRoomName = MCUConfig("Room Codes").GetString(codeRoom);
    }

    if(codeRoomName != "" && codePos != "")
    {
      MCUConferenceList & conferenceList = ep.GetConferenceManager().GetConferenceList();
      for(MCUConferenceList::shared_iterator it = conferenceList.begin(); it != conferenceList.end(); ++it)
      {
        Conference *conference = it.GetObject();

        if(conference->GetNumber() != codeRoomName)
          continue;

        MCUMemberList & memberList = conference->GetMemberList();
        for(MCUMemberList::shared_iterator it2 = memberList.begin(); it2 != memberList.end(); ++it2)
        {
          ConferenceMember * member = *it2;
          if(member->GetType() & MEMBER_TYPE_GSYSTEM)
            continue;
          MCUSimpleVideoMixer *mixer = ep.GetConferenceManager().FindVideoMixerWithLock(conference, member->GetVideoMixerNumber());
          if(mixer == NULL)
            continue;
          int pos = mixer->GetPositionNum(member->GetID());
          mixer->Unlock();
          if(pos < 0)
            continue;
          if(pos == atoi(codePos))
          {
            codeConferenceMember = member;
            codeMsg << "<br>-> action:"+codeAction+" room:"+codeRoomName+" pos:"+codePos+" found:" << member->GetName();
            break;
          }
        }
        break;
      }
    }

    if(codeConferenceMember != NULL)
    {
      if(codeAction == "close")
        codeConferenceMember->Close();
      else if(codeAction == "mute")
        codeConferenceMember->muteMask|=1;
      else if(codeAction == "unmute")
        codeConferenceMember->muteMask&=~1;
      codeMsg << "</font>";
      OpenMCU::Current().HttpWriteEvent(codeMsg);
      //OpenMCU::Current().HttpWriteEventRoom(codeMsg, conference->GetNumber());
    }
  } else {
    // the old "H.245/User Input Indication/DTMF" by kay27
    conferenceMember->SendUserInputIndication(dtmfBuffer);
  }
  dtmfBuffer = "";
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::ChangeWelcomeState(int newState)
{
  PWaitAndSignal m(connMutex);

  if(welcomeState != newState)
  {
    PTRACE(4, trace_section << "Entering welcome state " << newState);
    welcomeState = newState;
    wavePlayingInSameState = FALSE;
    OnWelcomeStateChanged();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::PlayWelcomeFile(BOOL useTheFile, PFilePath & fileToPlay)
{
  playFile.Close();

  wavePlayingInSameState = TRUE;

  if(useTheFile) {
    if(playFile.Open(fileToPlay, PFile::ReadOnly))
    {
      PTRACE(4, trace_section << "Playing welcome procedure file " << fileToPlay);
      return;
    }
    else
      PTRACE(3, trace_section << "Failed to play welcome procedure file " << fileToPlay);
   }

  // File not played, call the wave end callback anyway
  OnWelcomeWaveEnded();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnWelcomeStateChanged()
{
  PFilePath fn = OpenMCU::Current().GetConnectingWAVFile();

  switch(welcomeState) {

    case PlayingWelcome:
      // Welcome file not implemented yet
      PlayWelcomeFile(FALSE, fn);
      break;

    case PlayingConnecting:
      PlayWelcomeFile(FALSE, fn);
      //PlayWelcomeFile(OpenMCU::Current().GetConnectingWAVFile(fn), fn);
      break;

    case CompleteConnection:
      break;

    case JoinFailed:
    case ConferenceEnded:
      // Goodbye file not implemented yet
      PlayWelcomeFile(FALSE, fn);
      break;

    default:
      // Do nothing
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnWelcomeProcessing()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::OnWelcomeWaveEnded()
{
  switch(welcomeState) {

    case PlayingWelcome:
      ChangeWelcomeState(PlayingConnecting);
      break;

    case PlayingConnecting:
      ChangeWelcomeState(CompleteConnection);
      break;

    case JoinFailed:
    case ConferenceEnded:
      break;

    default:
      // Do nothing
      break;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUH323Connection::GetRemoteNumber()
{
  PString number;
  if(connectionType == CONNECTION_TYPE_H323)
  {
    if(ep.IsRegisteredWithGatekeeper() && remotePartyAddress.Left(4) == "url:")
    {
      PURL url(remotePartyAddress.Right(remotePartyAddress.GetLength()-4), "h323");
      number = url.GetUserName();
    }
    if(number == "")
      number = remotePartyNumber;
    if(number == "")
    {
      if(remoteAliasNames.GetSize() == 1)
        number = remoteAliasNames[0];
      else if(remoteAliasNames.GetSize() > 1)
        number = remoteAliasNames[1];
    }
    if(number == "")
      number = "undefined";
  }
  else
    number = MCUURL(remotePartyAddress).GetUserName();
  return number;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetRemoteName(const H323SignalPDU & pdu)
{
  // remotePartyNumber
  // Аккаунт - для получения настроек терминала
  if(callAnswered)
  {
    if(remotePartyNumber.IsEmpty())
      pdu.GetSourceE164(remotePartyNumber);
  }
  else
  {
    if(remoteAliasNames.GetSize() > 0)
      remotePartyNumber = remoteAliasNames[0];
    else
      remotePartyNumber = remotePartyAddress.Tokenise("@")[0];
  }

  // remotePartyName
  remotePartyName = pdu.GetQ931().GetDisplayName();
  if(remotePartyName.IsEmpty() && remoteAliasNames.GetSize() > 0)
    remotePartyName = remoteAliasNames[0];
  if(remotePartyName.IsEmpty())
    remotePartyName = remotePartyNumber;
  if(remotePartyName.IsEmpty())
    remotePartyName = signallingChannel->GetRemoteAddress().GetHostName();
  PTRACE(1, trace_section << "Set remotePartyName: " << remotePartyName);

  // ??? h323plus 1.25.0
  remotePartyNumber.Replace("h323:", "", TRUE, 0);
  remotePartyName.Replace("h323:", "", TRUE, 0);
  remotePartyAddress.Replace("h323:", "", TRUE, 0);
  for(int i = 0; i < remoteAliasNames.GetSize(); ++i)
    remoteAliasNames[i].Replace("h323:", "", TRUE, 0);

  if(remoteApplication.Find("MyPhone") != P_MAX_INDEX
     || remoteApplication.Find("Polycom ViaVideo\tRelease 8.0") != P_MAX_INDEX
    )
  {
    // convert
    remotePartyName = convert_cp1251_to_utf8(remotePartyName);
  }

  // Может быть пустым в GetSourceE164()
  if(callAnswered && remotePartyNumber.IsEmpty())
    remotePartyNumber = remotePartyName;

  // display name
  PString displayName = GetEndpointParam(DisplayNameKey);
  if(displayName != "")
  {
    remotePartyName = displayName;
    PTRACE(1, trace_section << "Set remotePartyName: " << remotePartyName);
  }

  SetMemberName();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SetMemberName()
{
  PString address;
  if(connectionType == CONNECTION_TYPE_SIP)
  {
    address = remotePartyAddress;
  }
  else if(connectionType == CONNECTION_TYPE_RTSP)
  {
    address = remotePartyAddress;
  }
  else
  {
    PString alias, host, port;
    if(remotePartyAddress.Left(4) == "url:" && ep.IsRegisteredWithGatekeeper())
    {
      PURL url(remotePartyAddress.Right(remotePartyAddress.GetLength()-4), "h323");
      host = url.GetHostName();
      port = url.GetPort();
      host.Replace("@","",TRUE,0);
    }
    else if(remotePartyAddress.Find("ip$") == P_MAX_INDEX && ep.IsRegisteredWithGatekeeper())
    {
      H323Gatekeeper *gk = ep.GetGatekeeper();
      PURL url(gk->GetName(), "h323");
      host = url.GetHostName();
      port = url.GetPort();
    } else {
      host = remotePartyAddress;
      PINDEX pos = host.Find("ip$");
      if(pos != P_MAX_INDEX) host = host.Mid(pos+3);
      port = host.Tokenise(":")[1];
      host = host.Tokenise(":")[0];
    }
    alias = GetRemoteNumber();

    //PRegularExpression RegEx("[^A-Za-z0-9._-]");
    //if(alias.FindRegEx(RegEx) != P_MAX_INDEX) alias = "invalid_name";

    address = "h323:"+alias+"@"+host;
    if(!HadAnsweredCall() && port != "") address += ":"+port;
  }

  memberName = remotePartyName+" ["+address+"]";

  PTRACE(1, trace_section << "SetMemberName remote account: " << GetRemoteNumber());
  PTRACE(1, trace_section << "SetMemberName remotePartyName: " << remotePartyName);
  PTRACE(1, trace_section << "SetMemberName remotePartyNumber: " << remotePartyNumber);
  PTRACE(1, trace_section << "SetMemberName remotePartyAddress: " << remotePartyAddress);
  PTRACE(1, trace_section << "SetMemberName remotePartyAliases: " << remoteAliasNames);
  PTRACE(1, trace_section << "SetMemberName memberName: " << memberName);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::SendUserInput(const PString & value)
{
  if(connectionType != CONNECTION_TYPE_H323)
    return;
  H323Connection::SendUserInput(value);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnIncomingAudio(const uint64_t & timestamp, const void * buffer, PINDEX amount, unsigned sampleRate, unsigned channels)
{
/*
  // If record file is open, write data to it
  if (recordFile.IsOpen()) {
    recordFile.Write(buffer, amount);

    recordDuration += amount / 2;
    if (recordDuration > recordLimit) {
      recordFile.Close();
      OnFinishRecording();
    }
    else {
      const WORD * samples = (const WORD *)buffer;
      PINDEX sampleCount = amount / 2;
      BOOL silence = TRUE;
      while (sampleCount-- > 0 && silence) {
        if (*samples > 100 || *samples < -100)
          silence = FALSE;
        ++samples;
      }
      if (!silence)
        recordSilenceCount = 0;
      else {
        recordSilenceCount += amount / 2;
        if ((recordSilenceThreshold > 0) && (recordSilenceCount >= recordSilenceThreshold)) {
          recordFile.Close();
          OnFinishRecording();
        }
      }
    }
  }

  else */ if (conferenceMember != NULL)
    conferenceMember->WriteAudio(timestamp, buffer, amount, sampleRate, channels);

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnOutgoingAudio(const uint64_t & timestamp, void * buffer, PINDEX amount, unsigned sampleRate, unsigned channels)
{
/*
  // When the prodedure begins, play the welcome file
  if (welcomeState == NotStartedYet) {
    ChangeWelcomeState(PlayingWelcome);
  }

  for (;;) {
    // Do actions that are not triggered by events
    OnWelcomeProcessing();

    // If a wave is not playing, we may continue now
    if (!playFile.IsOpen())
      break;

    // Wait for wave file completion
    if (playFile.Read(buffer, amount)) {
      int len = playFile.GetLastReadCount();
      if (len < amount) {
        memset(((BYTE *)buffer)+len, 0, amount-len);
      }
      //playDelay.Delay(amount/16);

      // Exit now since the buffer is ready
      return TRUE;
    }

    PTRACE(4, "MCU\tFinished playing file");
    playFile.Close();

    // Wave completed, if no event should be fired
    //  then we may continue now
    if(!wavePlayingInSameState)
      break;

    // Fire wave completion event
    OnWelcomeWaveEnded();

    // We should repeat the loop now because the callback
    //  above might have started a new wave file
  }
*/

  // If a we are connected to a conference and no wave
  //  is playing, read data from the conference
  if (conferenceMember != NULL) {
    conferenceMember->ReadAudio(timestamp, buffer, amount, sampleRate, channels);
    return TRUE;
  }

  // Generate silence
  return FALSE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

#if MCU_VIDEO

BOOL MCUH323Connection::OnOutgoingVideo(void * buffer, int width, int height, PINDEX & amount)
{
  if(conferenceMember != NULL)
    conferenceMember->ReadVideo(buffer, width, height, amount);
  else
    return FALSE;

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::OnIncomingVideo(const void * buffer, int width, int height, PINDEX amount)
{
  if(conferenceMember != NULL)
    conferenceMember->WriteVideo(buffer, width, height, amount);

  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL MCUH323Connection::GetPreMediaFrame(void * buffer, int width, int height, PINDEX & amount)
{
  return OpenMCU::Current().GetPreMediaFrame(buffer, width, height, amount);
}

#endif // MCU_VIDEO

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUConnection_ConferenceMember::MCUConnection_ConferenceMember(Conference * _conference, MCUH323EndPoint & _ep, const PString & _callToken, BOOL _isMCU)
  : ConferenceMember(_conference), ep(_ep)
{
  if(_isMCU) memberType = MEMBER_TYPE_MCU;
  callToken = _callToken;
  conference->AddMember(this);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

MCUConnection_ConferenceMember::~MCUConnection_ConferenceMember()
{
  if(conference)
    conference->RemoveMember(this);
  PTRACE(4, "MCUConnection_ConferenceMember deleted");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUConnection_ConferenceMember::Close()
{
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if (conn != NULL) {
    conn->ClearCall();
    conn->Unlock();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

PString MCUConnection_ConferenceMember::GetMonitorInfo(const PString & hdr)
{ 
  PStringStream output;
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if (conn != NULL) {
    output << hdr << "Remote Address: " << conn->GetRemotePartyAddress() << "\n"
           << hdr << "AudioCodecs: " << conn->GetAudioTransmitCodecName() << '/' << conn->GetAudioReceiveCodecName() << "\n"
#if MCU_VIDEO
           << hdr << "VideoCodecs: " << conn->GetVideoTransmitCodecName() << '/' << conn->GetVideoReceiveCodecName() << "\n"
#endif           
           ;
    conn->Unlock();
  }
  return output;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUConnection_ConferenceMember::SetName()
{
  if(memberType & MEMBER_TYPE_GSYSTEM)
    return;

  PTRACE(4,"H323\tSetName " << callToken);
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn == NULL)
    return;

  ConferenceMember * member = conn->GetConferenceMember();
  if(member == this || member == NULL)
  {
    name = conn->GetMemberName();
    nameID = MCUURL(name).GetMemberNameId();
    PTRACE(1, "SetName name: " << name);
  }
  else
    PTRACE(1, "MCU\tWrong connection in SetName for " << callToken);

  conn->Unlock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

// signal to codec plugin for disable(enable) decoding incoming video from unvisible(visible) member
void MCUConnection_ConferenceMember::SetFreezeVideo(BOOL disable) const
{
  if(memberType & MEMBER_TYPE_GSYSTEM)
    return;

  cout << id << "->SetFreezeVideo(" << disable << ")\n";
  PTRACE(5,id << "->SetFreezeVideo(" << disable << ")");
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn == NULL)
    return;

  ConferenceMember * member = conn->GetConferenceMember();
  if(member == this || member == NULL)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel *channel = conn->GetVideoReceiveChannel();
    if(channel)
      channel->SetFreeze(disable);
  }
  else
    PTRACE(1, "MCU\tWrong connection in SetFreezeVideo for " << callToken);

  conn->Unlock();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUConnection_ConferenceMember::SendUserInputIndication(const PString & str)
{
  if(memberType & MEMBER_TYPE_GSYSTEM)
    return;

  PTRACE(3, "Conference\tConnection " << id << " sending user indication " << str);
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn == NULL)
    return;

  PStringStream msg;
  PStringStream utfmsg;
  if(conn->GetRemoteApplication().Find("MyPhone")!=P_MAX_INDEX)
  {
    static const int table[128] = { // cp1251 -> utf8 translation based on http://www.linux.org.ru/forum/development/3968525
      0x82D0,0x83D0,  0x9A80E2,0x93D1,  0x9E80E2,0xA680E2,0xA080E2,0xA180E2,0xAC82E2,0xB080E2,0x89D0,0xB980E2,0x8AD0,0x8CD0,0x8BD0,0x8FD0,
      0x92D1,0x9880E2,0x9980E2,0x9C80E2,0x9D80E2,0xA280E2,0x9380E2,0x9480E2,0,       0xA284E2,0x99D1,0xBA80E2,0x9AD1,0x9CD1,0x9BD1,0x9FD1,
      0xA0C2,0x8ED0,  0x9ED1,  0x88D0,  0xA4C2,  0x90D2,  0xA6C2,  0xA7C2,  0x81D0,  0xA9C2,  0x84D0,0xABC2,  0xACC2,0xADC2,0xAEC2,0x87D0,
      0xB0C2,0xB1C2,  0x86D0,  0x96D1,  0x91D2,  0xB5C2,  0xB6C2,  0xB7C2,  0x91D1,  0x9684E2,0x94D1,0xBBC2,  0x98D1,0x85D0,0x95D1,0x97D1,
      0x90D0,0x91D0,  0x92D0,  0x93D0,  0x94D0,  0x95D0,  0x96D0,  0x97D0,  0x98D0,  0x99D0,  0x9AD0,0x9BD0,  0x9CD0,0x9DD0,0x9ED0,0x9FD0,
      0xA0D0,0xA1D0,  0xA2D0,  0xA3D0,  0xA4D0,  0xA5D0,  0xA6D0,  0xA7D0,  0xA8D0,  0xA9D0,  0xAAD0,0xABD0,  0xACD0,0xADD0,0xAED0,0xAFD0,
      0xB0D0,0xB1D0,  0xB2D0,  0xB3D0,  0xB4D0,  0xB5D0,  0xB6D0,  0xB7D0,  0xB8D0,  0xB9D0,  0xBAD0,0xBBD0,  0xBCD0,0xBDD0,0xBED0,0xBFD0,
      0x80D1,0x81D1,  0x82D1,  0x83D1,  0x84D1,  0x85D1,  0x86D1,  0x87D1,  0x88D1,  0x89D1,  0x8AD1,0x8BD1,  0x8CD1,0x8DD1,0x8ED1,0x8FD1
    };
    for(PINDEX i=0;i<str.GetLength();i++){
      unsigned int charcode=(BYTE)str[i];
      if(charcode&128){
        if((charcode=table[charcode&127])){
          utfmsg << (char)charcode << (char)(charcode >> 8);
          if(charcode >>= 16) utfmsg << (char)charcode;
        }
      } else utfmsg << (char)charcode;
    }
  }
  else
    utfmsg << str;

  msg << "<font color=blue><b>" << name << "</b>: " << utfmsg << "</font>";
  OpenMCU::Current().HttpWriteEvent(msg);

  if(conn->GetConferenceMember() != this && conn->GetConferenceMember() != NULL)
  {
    conn->Unlock();
    PTRACE(1, "MCU\tWrong connection in SendUserInputIndication for " << callToken);
    return;
  }

  PString sendmsg = "[" + conn->GetRemotePartyName() + "]: " + str;

  // unlock
  conn->Unlock();

  if(str.GetLength()<10)
  {
    iISequence << str.Trim();
    iISequence=iISequence.Right(10);
  }
  else
    iISequence=str.Trim();

  //cout << "*uii: " << iISequence << "\n";
  PINDEX hashPos=iISequence.FindLast("#");
  if(hashPos != P_MAX_INDEX)
  {
    PINDEX astPos=iISequence.FindLast("*");
    if(astPos < hashPos)
    {
      PINDEX astPos2=iISequence.Left(astPos-1).FindLast("*");
      PINDEX hashPos2=iISequence.Left(astPos-1).FindLast("#");
      if(astPos2!=P_MAX_INDEX) if((hashPos2==P_MAX_INDEX)||(hashPos2<astPos2)) astPos=astPos2;
      Conference * conference=GetConference();
      if(conference!=NULL)
      {
        conference->HandleFeatureAccessCode(*this,iISequence(astPos+1,hashPos-1));
        iISequence=iISequence.Mid(hashPos+1,P_MAX_INDEX);
        return;
      }
    }
  }

  if(conference)
  {
    MCUMemberList & memberList = conference->GetMemberList();
    for(MCUMemberList::shared_iterator it = memberList.begin(); it != memberList.end(); ++it)
    {
      ConferenceMember * member = *it;
      if(member == this || member->GetType() & MEMBER_TYPE_GSYSTEM)
        continue;
      member->OnReceivedUserInputIndication(sendmsg);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUConnection_ConferenceMember::SetChannelPauses(unsigned mask)
{
  unsigned sumMask = 0;
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn == NULL) return;
  PString room; { Conference * c = ConferenceMember::conference; if(c) room = c->GetNumber(); }
  if(mask & 1)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetAudioReceiveChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelInactive);
      channel->SetFreeze(true);
      ConferenceMember::muteMask |= 1;
      sumMask |= 1;
    }
  }
  if(mask & 2)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetAudioTransmitChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelInactive);
      channel->SetFreeze(true);
      ConferenceMember::muteMask |= 2;
      sumMask |= 2;
    }
  }
  if(mask & 4)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetVideoReceiveChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelInactive);
      channel->SetFreeze(true);
      ConferenceMember::muteMask |= 4;
      sumMask |= 4;
    }
  }
  if(mask & 8)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetVideoTransmitChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelInactive);
      channel->SetFreeze(true);
      ConferenceMember::muteMask |= 8;
      sumMask |= 8;
    }
  }
  conn->Unlock();
  PStringStream cmd; cmd << "imute(" << dec << (long)id << "," << sumMask << ")";
  if(room.IsEmpty()) OpenMCU::Current().HttpWriteCmd(cmd); else OpenMCU::Current().HttpWriteCmdRoom(cmd, room);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUConnection_ConferenceMember::UnsetChannelPauses(unsigned mask)
{
  unsigned sumMask = 0;
  MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
  if(conn == NULL) return;
  PString room; { Conference * c = ConferenceMember::conference; if(c) room = c->GetNumber(); }
  if(mask & 1)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetAudioReceiveChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelActive);
      channel->SetFreeze(false);
      ConferenceMember::muteMask &= ~1;
      sumMask |= 1;
    }
  }
  if(mask & 2)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetAudioTransmitChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelActive);
      channel->SetFreeze(false);
      ConferenceMember::muteMask &= ~2;
      sumMask |= 2;
    }
  }
  if(mask & 4)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetVideoReceiveChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelActive);
      channel->SetFreeze(false);
      ConferenceMember::muteMask &= ~4;
      sumMask |= 4;
    }
  }
  if(mask & 8)
  {
    PWaitAndSignal m(conn->GetChannelsMutex());
    MCU_RTPChannel * channel = conn->GetVideoTransmitChannel();
    if(channel)
    {
      channel->SendMiscIndication(H245_MiscellaneousIndication_type::e_logicalChannelActive);
      channel->SetFreeze(false);
      ConferenceMember::muteMask &= ~8;
      sumMask |= 8;
    }
  }
  conn->Unlock();
  PStringStream cmd; cmd << "iunmute(" << dec << (long)id << "," << sumMask << ")";
  if(room.IsEmpty()) OpenMCU::Current().HttpWriteCmd(cmd); else OpenMCU::Current().HttpWriteCmdRoom(cmd, room);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void MCUH323Connection::LogCall(const BOOL accepted)
{
  if(!conference || !conferenceMember)
    return;

  PStringStream stringStream, timeStream;
  timeStream << GetConnectionStartTime().AsString("hh:mm:ss");
  stringStream << " caller: " << memberName
               << " room: " << ((conference != NULL) ? conference->GetNumber() : PString());

  if(accepted)
  {
    PStringStream connectionDuration;
    connectionDuration << setprecision(0) << setw(5) << (PTime() - GetConnectionStartTime());
    OpenMCU::Current().LogMessage(timeStream + stringStream	+ " duration:" + connectionDuration);
  }
  else
    OpenMCU::Current().LogMessage(timeStream + " Call denied:" + stringStream);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

OutgoingAudio::OutgoingAudio(MCUH323Connection & _conn, unsigned int _sampleRate, unsigned _channels)
  : conn(_conn), sampleRate(_sampleRate), channels(_channels)
{
  os_handle = 0;
  lastReadCount = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void OutgoingAudio::CreateSilence(void * buffer, PINDEX amount)
{
  memset(buffer, 0, amount);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL OutgoingAudio::Read(void * buffer, PINDEX amount)
{
  PWaitAndSignal mutexR(audioChanMutex);

  if(!IsOpen())
    return FALSE;

  if(lastReadCount == 0)
    delay.Restart();
  else
  {
    unsigned delay_us = 1000000 * amount / (sampleRate * channels * 2);
    delay.DelayUsec(delay_us);
  }

  if(!conn.OnOutgoingAudio(delay.GetDelayTimestampUsec(), buffer, amount, sampleRate, channels))
    CreateSilence(buffer, amount);

  lastReadCount = amount;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL OutgoingAudio::Close()
{
  if(!IsOpen())
    return FALSE;

  PWaitAndSignal mutexC(audioChanMutex);
  os_handle = -1;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

IncomingAudio::IncomingAudio(MCUH323Connection & _conn, unsigned int _sampleRate, unsigned _channels)
  : conn(_conn), sampleRate(_sampleRate), channels(_channels)
{
  os_handle = 0;
  lastWriteCount = 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL IncomingAudio::Write(const void * buffer, PINDEX amount)
{
  PWaitAndSignal mutexW(audioChanMutex);

  if(!IsOpen())
    return FALSE;

  if(lastWriteCount == 0)
    delay.Restart();

  unsigned delay_us = 1000000 * amount / (sampleRate * channels * 2);
  unsigned jitter_us = 0;
  uint64_t timestamp = delay.GetDelayTimestampUsec(delay_us, jitter_us);

  conn.OnIncomingAudio(timestamp, buffer, amount, sampleRate, channels);

  delay.DelayUsec(delay_us);

  lastWriteCount = amount;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

BOOL IncomingAudio::Close()
{
  if (!IsOpen())
    return FALSE;

  PWaitAndSignal mutexA(audioChanMutex);
  os_handle = -1;
  return TRUE;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void ConnectionMonitor::Main()
{
  running = TRUE;
  while(running)
  {
    Sleep(1000);
    if(!running)
      break;

    for(MCUConnectionList::shared_iterator it = monitorList.begin(); it != monitorList.end(); ++it)
    {
      PString callToken = it.GetName();
      MCUH323Connection * conn = ep.FindConnectionWithLock(callToken);
      if(conn == NULL)
      {
        monitorList.Erase(it);
        continue;
      }
      int ret = Perform(conn);
      if(ret == 1)
        conn->ClearCall();
      conn->Unlock();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void ConnectionMonitor::AddConnection(const PString & callToken)
{
  long id = monitorList.GetNextID();
  monitorList.Insert(NULL, id, callToken);
  monitorList.Release(id);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void ConnectionMonitor::RemoveConnection(const PString & callToken)
{
  MCUConnectionList::shared_iterator it = monitorList.Find(callToken);
  if(it != monitorList.end())
    monitorList.Erase(it);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int ConnectionMonitor::Perform(MCUH323Connection * conn)
{
  int ret = 0;
  if((ret = RTPTimeoutMonitor(conn)) != 0)
    return ret;

  return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

int ConnectionMonitor::RTPTimeoutMonitor(MCUH323Connection * conn)
{
  conn->rtpInputTimeout = conn->GetEndpointParam(RTPInputTimeoutKey, PString(DefaultRTPInputTimeout)).AsInteger();
  // Disable  == 0
  if(conn->rtpInputTimeout <= 0)
    return 0;

  MCU_RTP_UDP *as = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultAudioSessionID);
  MCU_RTP_UDP *vs = (MCU_RTP_UDP *)conn->GetSession(RTP_Session::DefaultVideoSessionID);
  int count = 0;
  if(as) count += as->GetPacketsReceived() + as->GetRtpcReceived();
  if(vs) count += vs->GetPacketsReceived() + vs->GetRtpcReceived();
  if((as || vs) && count == conn->rtpInputBytes)
  {
    conn->rtpInputLostInterval++;
  } else {
    conn->rtpInputLostInterval = 0;
    conn->rtpInputBytes = count;
  }
  if(conn->rtpInputLostInterval >= conn->rtpInputTimeout)
  {
    PTRACE(1, "MCU\tConnection: " << conn->GetCallToken() << ", " << conn->rtpInputTimeout << " sec timeout waiting incoming stream data.");
    return 1; // leave
  }

  return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void GatekeeperMonitor::Main()
{
  for(;;)
  {
    if(terminate)
      break;

    if(ep.GetListeners().IsEmpty())
    {
      MCUTime::Sleep(500);
      continue;
    }

    // после отмены регистрации следующая попытка будет через Time To Live
    if(ep.GetGatekeeper() && !ep.IsRegisteredWithGatekeeper())
      ep.RemoveGatekeeper();

    PTime now;
    PStringStream event;
    if(mode == "Find gatekeeper")
    {
      if(!ep.GetGatekeeper() && now > nextRetryTime)
      {
        if(ep.DiscoverGatekeeper(new H323TransportUDP(ep)))
        {
          event << "GatekeeperMonitor: Found gatekeeper " << *ep.GetGatekeeper();
        }
        else
        {
          nextRetryTime = now + retryInterval;
          event << "GatekeeperMonitor: No gatekeeper found";
        }
      }
    }
    else if(mode == "Use gatekeeper")
    {
      if(!ep.GetGatekeeper() && now > nextRetryTime)
      {
        PString address = MCUConfig("H323 Parameters").GetString(GatekeeperKey);
        if(ep.SetGatekeeper(address, new H323TransportUDP(ep)))
        {
          event << "GatekeeperMonitor: Registered with gatekeeper " << *ep.GetGatekeeper();
        }
        else
        {
          nextRetryTime = now + retryInterval;
          event << "GatekeeperMonitor: Error registering with gatekeeper " << address;
        }
      }
    }

    if(event != "")
    {
      MCUTRACE(1, event);
      OpenMCU::Current().HttpWriteEvent("<font color=blue>"+event+"</font>");
    }

    MCUTime::Sleep(1000);
  }

  if(ep.GetGatekeeper())
  {
    PStringStream event;
    event << "GatekeeperMonitor: Remove gatekeeper " << *ep.GetGatekeeper();
    MCUTRACE(1, event);
    OpenMCU::Current().HttpWriteEvent("<font color=blue>"+event+"</font>");
    ep.RemoveGatekeeper();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientMStream                                                     //
//                                                                      //
// Author: Fabrizio Furano (INFN Padova, 2006)                          //
//                                                                      //
// Helper code for XrdClient to handle multistream behavior             //
// Functionalities dealing with                                         //
//  mstream creation on init                                            //
//  decisions to add/remove one                                         //
//                                                                      //
//////////////////////////////////////////////////////////////////////////


#include "XrdClient/XrdClientMStream.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "XrdClient/XrdClientDebug.hh"

int XrdClientMStream::EstablishParallelStreams(XrdClientConn *cliconn) {
    int mx = EnvGetLong(NAME_MULTISTREAMCNT);
    int i, res;
    int wan_port = 0, wan_window = 0;


    if (mx <= 1) return 1;

    // Query the server config, for the WAN port and the windowsize
    char *qryitems = (char *)"wan_port wan_window";
    ClientRequest qryRequest;
    char *qryResp = 0;
    memset( &qryRequest, 0, sizeof(qryRequest) );

    cliconn->SetSID(qryRequest.header.streamid);
    qryRequest.header.requestid = kXR_query;
    qryRequest.query.infotype = kXR_Qconfig;
    qryRequest.header.dlen = strlen(qryitems);

    res =  cliconn->SendGenCommand(&qryRequest, qryitems, (void **)&qryResp, 0,
				   true, (char *)"QueryConfig");

    if (res && (cliconn->LastServerResp.status == kXR_ok) &&
	qryResp && cliconn->LastServerResp.dlen) {

      sscanf(qryResp, "%d\n%d",
	     &wan_port,
	     &wan_window);

      delete qryResp;

      Info(XrdClientDebug::kUSERDEBUG,
	   "XrdClientMStream::EstablishParallelStreams", "Server WAN parameters: port=" << wan_port << " windowsize=" << wan_window );
    }


    for (i = 0; i < mx; i++) {
	Info(XrdClientDebug::kHIDEBUG,
	     "XrdClientMStream::EstablishParallelStreams", "Trying to establish " << i+1 << "th substream." );
	// If something goes wrong, stop adding new streams
	if (AddParallelStream(cliconn, wan_port, wan_window))
	    break;

    }


    return i;
}

// Add a parallel stream to the pool used by the given client inst
// Returns 0 if ok
int XrdClientMStream::AddParallelStream(XrdClientConn *cliconn, int port, int windowsz) {

    // Get the XrdClientPhyconn to be used
    XrdClientPhyConnection *phyconn =
	ConnectionManager->GetConnection(cliconn->GetLogConnID())->GetPhyConnection();


    // If the phyconn already has all the needed streams... exit
    if (phyconn->GetSockIdCount() > EnvGetLong(NAME_MULTISTREAMCNT)) return 0;

    // Connect a new connection, get the socket fd
    if (phyconn->TryConnectParallelStream(port, windowsz) < 0) return -1;

    // The connection now is here with a temp id XRDCLI_PSOCKTEMP
    // Do the handshake
    ServerInitHandShake xbody;
    if (phyconn->DoHandShake(xbody, XRDCLI_PSOCKTEMP) == kSTError) return -1;

    // After the handshake make the reader thread aware of the new stream
    phyconn->ReinitFDTable();

    // Send the kxr_bind req to get a new substream id
    int newid = -1;
    int res = -1;
    if (BindPendingStream(cliconn, XRDCLI_PSOCKTEMP, newid) &&
	phyconn->IsValid() ) {
      
	// Everything ok, Establish the new connection with the new id
	res = phyconn->EstablishPendingParallelStream(newid);
    
	if (res) {
	    // If the establish failed we have to remove the pending stream
	    RemoveParallelStream(cliconn, XRDCLI_PSOCKTEMP);
	    return res;
	}

    }
    else {
	// If the bind failed we have to remove the pending stream
	RemoveParallelStream(cliconn, XRDCLI_PSOCKTEMP);
	return -1;
    }

    Info(XrdClientDebug::kHIDEBUG,
	 "XrdClientMStream::EstablishParallelStreams", "Substream added." );
    return 0;

}

// Remove a parallel stream to the pool used by the given client inst
int XrdClientMStream::RemoveParallelStream(XrdClientConn *cliconn, int substream) {

  // Get the XrdClientPhyconn to be used
  XrdClientLogConnection *log = ConnectionManager->GetConnection(cliconn->GetLogConnID());
  if (!log) return 0;

  XrdClientPhyConnection *phyconn = log->GetPhyConnection();
    
  if (phyconn) 
    phyconn->RemoveParallelStream(substream);
    
  return 0;
    
}



// Binds the pending temporary parallel stream to the current session
// Returns the substreamid assigned by the server into newid
bool XrdClientMStream::BindPendingStream(XrdClientConn *cliconn, int substreamid, int &newid) {
    bool res = false;

    // Prepare request
    ClientRequest bindFileRequest;
    XrdClientConn::SessionIDInfo sess;
    ServerResponseBody_Bind bndresp;

    // Note: this phase has not to overwrite XrdClientConn::LastServerresp
    struct ServerResponseHeader
	LastServerResptmp = cliconn->LastServerResp;

    // Get the XrdClientPhyconn to be used
    XrdClientPhyConnection *phyconn =
	ConnectionManager->GetConnection(cliconn->GetLogConnID())->GetPhyConnection();
    phyconn->ReinitFDTable();

    cliconn->GetSessionID(sess);

    memset( &bindFileRequest, 0, sizeof(bindFileRequest) );
    cliconn->SetSID(bindFileRequest.header.streamid);
    bindFileRequest.bind.requestid = kXR_bind;
    memcpy( bindFileRequest.bind.sessid, sess.id, sizeof(sess.id) );
   

    // The request has to be sent through the stream which has to be bound!
    res =  cliconn->SendGenCommand(&bindFileRequest, 0, 0, (void *)&bndresp,
				   FALSE, (char *)"Bind", substreamid);

    if (res && (cliconn->LastServerResp.status == kXR_ok)) newid = bndresp.substreamid;

    cliconn->LastServerResp = LastServerResptmp;

    return res;

}



// This splits a long requests into many smaller requests, to be sent in parallel
//  through multiple streams
// Returns false if the chunk is not worth splitting
bool XrdClientMStream::SplitReadRequest(XrdClientConn *cliconn, kXR_int64 offset, kXR_int32 len,
			     XrdClientVector<XrdClientMStream::ReadChunk> &reqlists) {

    int spltsize = DFLT_MULTISTREAMSPLITSIZE;
   

//     // Let's try to distribute the load into maximum sized chunks
//     if (cliconn->GetParallelStreamCount() > 1)
//       spltsize = xrdmax(DFLT_MULTISTREAMSPLITSIZE,
//                         len / (cliconn->GetParallelStreamCount()) + 1);

//     for (kXR_int32 pp = 0; pp < len; pp += spltsize) {
//       ReadChunk ck;

//       ck.offset = pp+offset;
//       ck.len = xrdmin(len - pp, spltsize);
//       ck.streamtosend = cliconn->GetParallelStreamToUse();

//       reqlists.Push_back(ck);

//     }
    



    int reqsperstream = 2;
    // Let's try to distribute the load into maximum sized chunks
    if (cliconn->GetParallelStreamCount() > 1) {

      // We start seeing which length we get trying to fill all the
      // available slots ( per stream)
      int candlen = xrdmax(DFLT_MULTISTREAMSPLITSIZE,
			   len / (reqsperstream * (cliconn->GetParallelStreamCount()-1)) + 1);

      // We don't want blocks smaller than a min value
      // If this is the case we consider only one slot per stream
      if (candlen < DFLT_MULTISTREAMSPLITSIZE) {
	spltsize = xrdmax(DFLT_MULTISTREAMSPLITSIZE,
			  len / (cliconn->GetParallelStreamCount()-1) + 1);
	reqsperstream = 1;
      }
      else spltsize = candlen;

//      spltsize = min(256*1024, spltsize);
    }
    else spltsize = len;

    for (kXR_int32 pp = 0; pp < len; pp += spltsize) {
      ReadChunk ck;

      ck.offset = pp+offset;
      ck.len = xrdmin(len - pp, spltsize);
      ck.streamtosend = cliconn->GetParallelStreamToUse(reqsperstream);

      reqlists.Push_back(ck);

    }

    return true;
}

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <memory>
#include <utility>

#include "resip/stack/Helper.hxx"
#include "resip/stack/SendData.hxx"
#include "resip/stack/SipMessage.hxx"
#include "resip/stack/UdpTransport.hxx"
#include "rutil/Data.hxx"
#include "rutil/DnsUtil.hxx"
#include "rutil/Logger.hxx"
#include "rutil/Socket.hxx"
#include "rutil/WinLeakCheck.hxx"
#include "rutil/compat.hxx"
#include "rutil/stun/Stun.hxx"

#ifdef USE_SIGCOMP
#include <osc/Stack.h>
#include <osc/StateChanges.h>
#include <osc/SigcompMessage.h>
#endif

#define RESIPROCATE_SUBSYSTEM Subsystem::TRANSPORT

using namespace std;
using namespace resip;

UdpTransport::UdpTransport(Fifo<TransactionMessage>& fifo,
                           int portNum,
                           IpVersion version,
                           StunSetting stun,
                           const Data& pinterface,
                           AfterSocketCreationFuncPtr socketFunc,
                           Compression &compression,
                           unsigned transportFlags)
   : InternalTransport(fifo, portNum, version, pinterface, socketFunc, compression, transportFlags),  
     mSigcompStack(nullptr),
     mStunSetting(stun),
     mExternalUnknownDatagramHandler(nullptr),
     mInWritable(false)
{
   mPollEventCnt = 0;
   mTxTryCnt = mTxMsgCnt = mTxFailCnt = 0;
   mRxTryCnt = mRxMsgCnt = mRxKeepaliveCnt = mRxTransactionCnt = 0;
   mTuple.setType(UDP);
   mFd = InternalTransport::socket(transport(), version);
   mTuple.mFlowKey=(FlowKey)mFd;
   bind();      // also makes it non-blocking

   InfoLog (<< "Creating UDP transport host=" << pinterface
            << " port=" << mTuple.getPort()
            << " ipv4=" << bool(version==V4) );

#ifdef USE_SIGCOMP
   if (mCompression.isEnabled())
   {
      DebugLog (<< "Compression enabled for transport: " << *this);
      mSigcompStack = new osc::Stack(mCompression.getStateHandler());
      mCompression.addCompressorsToStack(mSigcompStack);
   }
   else
   {
      DebugLog (<< "Compression disabled for transport: " << *this);
   }
#else
   DebugLog (<< "No compression library available: " << *this);
#endif
   mTxFifo.setDescription("UdpTransport::mTxFifo");
}

UdpTransport::~UdpTransport()
{
   InfoLog(<< "Shutting down " << mTuple
           <<" tf="<<mTransportFlags<<" evt="<<(mPollGrp?1:0)
           <<" stats:"
           <<" poll="<<mPollEventCnt
           <<" txtry="<<mTxTryCnt
           <<" txmsg="<<mTxMsgCnt
           <<" txfail="<<mTxFailCnt
           <<" rxtry="<<mRxTryCnt
           <<" rxmsg="<<mRxMsgCnt
           <<" rxka="<<mRxKeepaliveCnt
           <<" rxtr="<<mRxTransactionCnt
           );
#ifdef USE_SIGCOMP
   delete mSigcompStack;
#endif
   setPollGrp(nullptr);
}

void
UdpTransport::setPollGrp(FdPollGrp *grp)
{
   if(mPollGrp)
   {
      mPollGrp->delPollItem(mPollItemHandle);
      mPollItemHandle=nullptr;
   }

   if(mFd!=INVALID_SOCKET && grp)
   {
      mPollItemHandle = grp->addPollItem(mFd, FPEM_Read, this);
      // above released by InternalTransport destructor
      // ?bwc? Is this really a good idea? If the InternalTransport d'tor is
      // freeing this, shouldn't InternalTransport::setPollGrp() handle 
      // creating it?
   }

   InternalTransport::setPollGrp(grp);
}


/**
 * Called after a message is added. Could try writing it now.
 */
void
UdpTransport::process() 
{
   if ( (mTransportFlags & RESIP_TRANSPORT_FLAG_TXNOW)!= 0 )
   {
       processTxAll();
       // FALLTHRU to code below in case queue not-empty
       // shouldn't ever happen (with current code)
       // but in future we may throttle transmits
   }

   if ( mPollGrp )
   {
       updateEvents();
   }

   mStateMachineFifo.flush();
}

void
UdpTransport::updateEvents()
{
   //assert( mPollGrp );
   bool haveMsg = mTxFifoOutBuffer.messageAvailable();
   if ( !mInWritable && haveMsg )
   {
      mPollGrp->modPollItem(mPollItemHandle, FPEM_Read|FPEM_Write);
      mInWritable = true;
   }
   else if ( mInWritable && !haveMsg )
   {
      mPollGrp->modPollItem(mPollItemHandle, FPEM_Read);
      mInWritable = false;
   }
}


void
UdpTransport::processPollEvent(FdPollEventMask mask)
{
   ++mPollEventCnt;
   if ( mask & FPEM_Error )
   {
      resip_assert(0);
   }
   if ( mask & FPEM_Write )
   {
      processTxAll();
      updateEvents();   // turn-off writability
   }
   if ( mask & FPEM_Read )
   {
      processRxAll();
   }
   mStateMachineFifo.flush();
}

/**
   If we return true, the TransactionController will set the timeout
   to zero so that process() is called immediately. We don't want this;
   instead, we depend upon the writable-socket callback (fdset or poll).
**/
bool
UdpTransport::hasDataToSend() const
{
   return false;
}

void
UdpTransport::buildFdSet( FdSet& fdset )
{
   fdset.setRead(mFd);

   if (mTxFifoOutBuffer.messageAvailable())
   {
      fdset.setWrite(mFd);
   }
}

void
UdpTransport::process(FdSet& fdset)
{
   // pull buffers to send out of TxFifo
   // receive datagrams from fd
   // preparse and stuff into RxFifo
   if (fdset.readyToWrite(mFd))
   {
      processTxAll();
   }

   if ( fdset.readyToRead(mFd) )
   {
      processRxAll();
   }
   mStateMachineFifo.flush();
}

/**
 * Added support for TXNOW and TXALL. Generally only makes sense
 * to specify one of these. Limited testing shows limited performance
 * gain from either of these: the socket-event overhead appears tiny.
 */
void
UdpTransport::processTxAll()
{
   SendData *msg;
   ++mTxTryCnt;
   while ( (msg=mTxFifoOutBuffer.getNext(RESIP_FIFO_NOWAIT)) != nullptr)
   {
      processTxOne(msg);
      // With UDP we don't need to worry about write blocking (I hope)
      if ( (mTransportFlags & RESIP_TRANSPORT_FLAG_TXALL)==0 )
      {
         break;
      }
   }
}

void
UdpTransport::processTxOne(SendData *data)
{
   resip_assert(data);
   if(data->command != SendData::NoCommand)
   {
      // We don't handle any special SendData commands in the UDP transport yet.
      return;
   }
   ++mTxMsgCnt;
   std::unique_ptr<SendData> sendData(data);
   //DebugLog (<< "Sent: " <<  sendData->data);
   //DebugLog (<< "Sending message on udp.");
   resip_assert( sendData->destination.getPort() != 0 );

   const sockaddr& addr = sendData->destination.getSockaddr();
   int expected;
   int count;

#ifdef USE_SIGCOMP
   // If message needs to be compressed, compress it here.
   if (mSigcompStack &&
       sendData->sigcompId.size() > 0 &&
       !sendData->isAlreadyCompressed )
   {
       osc::SigcompMessage *sm = mSigcompStack->compressMessage
         (sendData->data.data(), sendData->data.size(),
          sendData->sigcompId.data(), sendData->sigcompId.size(),
          isReliable());

       DebugLog (<< "Compressed message from "
                 << sendData->data.size() << " bytes to "
                 << sm->getDatagramLength() << " bytes");

       expected = sm->getDatagramLength();

       count = sendto(mFd,
                      sm->getDatagramMessage(),
                      sm->getDatagramLength(),
                      0, // flags
                      &addr, sendData->destination.length());
       delete sm;
   }
   else
#endif
   {
       expected = (int)sendData->data.size();
       count = sendto(mFd,
                      sendData->data.data(), (int)sendData->data.size(),
                      0, // flags
                      &addr, (int)sendData->destination.length());
   }

   if ( count == SOCKET_ERROR )
   {
      int e = getErrno();
      error(e);
      InfoLog (<< "Failed (" << e << ") sending to " << sendData->destination);
      fail(sendData->transactionId);
      ++mTxFailCnt;
   }
   else
   {
      if (count != expected)
      {
         ErrLog (<< "UDPTransport - send buffer full" );
         fail(sendData->transactionId);
      }
   }
}

/**
 * Add options RXALL (to try receive all readable data).
 * With RXALL, every read cycle will have end with an EAGAIN read.
 * Testing in very limited cases shows marginal (5%) performance improvements.
 * Probably "real" traffic (that is bursty) would more impact.
 */
void
UdpTransport::processRxAll()
{
   ++mRxTryCnt;
   for (;;)
   {
      // TBD: check StateMac capacity
      Tuple sender(mTuple);
      const int len = processRxRecv(sender);
      if (len <= 0)
      {
         break;
      }
      ++mRxMsgCnt;
      processRxParse(len, sender);
      if ( (mTransportFlags & RESIP_TRANSPORT_FLAG_RXALL) == 0 )
      {
         break;
      }
   }
}

/*
 * Receive from socket and store results into {buffer}. Updates
 * {sender} with who sent the packet.
 * Return length of data read:
 *  0 if no data read and no more data to read (EAGAIN)
 *  >0 if data read and may be more data to read
**/
int
UdpTransport::processRxRecv(Tuple& sender)
{
   for (;;) 
   {
      // !jf! how do we tell if it discarded bytes
      // !ah! we use the len-1 trick :-(
      socklen_t slen = sender.length();
      int len = recvfrom( mFd,
                          mRxBuffer.data(),
                          MaxMessageSize,
                          0 /*flags */,
                          &sender.getMutableSockaddr(),
                          &slen);
      if ( len == SOCKET_ERROR )
      {
         int err = getErrno();
         if ( err != EAGAIN && err != EWOULDBLOCK ) // Treat EGAIN and EWOULDBLOCK as the same: http://stackoverflow.com/questions/7003234/which-systems-define-eagain-and-ewouldblock-as-different-values
         {
            error( err );
         }
         len = 0;
      }
      if (len + 1 >= MaxMessageSize)
      {
         InfoLog( << "Datagram exceeded max length " << MaxMessageSize);
         continue;
      }
      return len;
   }
}


/**
 * Parse the contents of {mRxBuffer} and do something with it.
**/
void
UdpTransport::processRxParse(int len, const Tuple& sender)
{
   //handle incoming CRLFCRLF keep-alive packets
   if (len == 4 &&
       strncmp(mRxBuffer.data(), Symbols::CRLFCRLF, len) == 0)
   {
      StackLog(<<"Throwing away incoming firewall keep-alive");
      ++mRxKeepaliveCnt;
      return;
   }

   // this must be a STUN response (or garbage)
   if (mRxBuffer[0] == 1 && mRxBuffer[1] == 1 && ipVersion() == V4)
   {
      StunMessage resp;

      resip::Lock lock(myMutex);

      // Change the stored stun result to parse failed since we now have response
      // Once parsing is successful below, the return code will be updated
      mStunResult = StunResultResponseParseFailed;

      if (stunParseMessage(mRxBuffer.data(), len, resp, false))
      {
         in_addr sin_addr;
         // Use XorMappedAddress if present - if not use MappedAddress
         if(resp.hasXorMappedAddress)
         {
            uint16_t id16 = resp.msgHdr.id.octet[0]<<8
                          | resp.msgHdr.id.octet[1];
            uint32_t id32 = resp.msgHdr.id.octet[0]<<24
                          | resp.msgHdr.id.octet[1]<<16
                          | resp.msgHdr.id.octet[2]<<8
                          | resp.msgHdr.id.octet[3];
            resp.xorMappedAddress.ipv4.port = resp.xorMappedAddress.ipv4.port^id16;
            resp.xorMappedAddress.ipv4.addr = resp.xorMappedAddress.ipv4.addr^id32;

#if defined(WIN32)
            sin_addr.S_un.S_addr = htonl(resp.xorMappedAddress.ipv4.addr);
#else
            sin_addr.s_addr = htonl(resp.xorMappedAddress.ipv4.addr);
#endif
            mStunMappedAddress = Tuple(sin_addr,resp.xorMappedAddress.ipv4.port, UDP);
            mStunResult = StunResultSuccess;
         }
         else if(resp.hasMappedAddress)
         {
#if defined(WIN32)
            sin_addr.S_un.S_addr = htonl(resp.mappedAddress.ipv4.addr);
#else
            sin_addr.s_addr = htonl(resp.mappedAddress.ipv4.addr);
#endif
            mStunMappedAddress = Tuple(sin_addr,resp.mappedAddress.ipv4.port, UDP);
            mStunResult = StunResultSuccess;
         }
      }
      return;
   }

   // this must be a STUN request (or garbage)
   if (mRxBuffer[0] == 0 && mRxBuffer[1] == 1 && ipVersion() == V4)
   {
      // Drop stun requests unless StunEnabled is set and return false to indicate
      // we did not consume the buffer
      if (mStunSetting == StunDisabled) return;

      bool changePort = false;
      bool changeIp = false;

      StunAddress4 myAddr;
      const sockaddr_in& bi = (const sockaddr_in&)boundInterface();
      myAddr.addr = ntohl(bi.sin_addr.s_addr);
      myAddr.port = ntohs(bi.sin_port);

      StunAddress4 from; // packet source
      const sockaddr_in& fi = (const sockaddr_in&)sender.getSockaddr();
      from.addr = ntohl(fi.sin_addr.s_addr);
      from.port = ntohs(fi.sin_port);

      StunMessage resp;
      StunAddress4 dest;
      StunAtrString hmacPassword;
      hmacPassword.sizeValue = 0;

      StunAddress4 secondary;
      secondary.port = 0;
      secondary.addr = 0;

      bool ok = stunServerProcessMsg( mRxBuffer.data(), len, // input buffer
                                      from,  // packet source
                                      secondary, // not used
                                      myAddr, // address to fill into response
                                      myAddr, // not used
                                      &resp, // stun response
                                      &dest, // where to send response
                                      &hmacPassword, // not used
                                      &changePort, // not used
                                      &changeIp, // not used
                                      false ); // logging

      if (ok)
      {
         DebugLog(<<"Got UDP STUN keepalive. Sending response...");
         char* response = new char[STUN_MAX_MESSAGE_SIZE];
         int rlen = stunEncodeMessage( resp,
                                       response,
                                       STUN_MAX_MESSAGE_SIZE,
                                       hmacPassword,
                                       false );
         SendData* stunResponse = new SendData(sender, response, rlen);
         mTxFifo.add(stunResponse);
      }
      return;
   }

   processRxParseSip(mRxBuffer.data(), len, sender);
}

void
UdpTransport::processRxParseSip(char* buffer, int len, const Tuple& sender)
{
#ifdef USE_SIGCOMP
   osc::StateChanges *sc = 0;
#endif

   // Attempt to decode SigComp message, if appropriate.
   if ((buffer[0] & 0xf8) == 0xf8)
   {
      if (!mCompression.isEnabled())
      {
         InfoLog(<< "Discarding unexpected SigComp Message");
         return;
      }
#ifdef USE_SIGCOMP
      const size_t uncompressedLength = mSigcompStack->uncompressMessage(buffer, len, mRxUncompressedBuffer.data(), mRxUncompressedBuffer.size(), sc);

      DebugLog (<< "Uncompressed message from "
               << len << " bytes to "
               << uncompressedLength << " bytes");

      osc::SigcompMessage *nack = mSigcompStack->getNack();

      if (nack)
      {
         mTxFifo.add(new SendData(sender,
                                  Data(nack->getDatagramMessage(),
                                       nack->getDatagramLength()),
                                  Data::Empty,
                                  Data::Empty,
                                  true));
         delete nack;
      }

      buffer = mRxUncompressedBuffer.data();
      len = uncompressedLength;
#endif
   }

   auto* const msgBuffer = MsgHeaderScanner::allocateBuffer(len);
   memcpy(msgBuffer, buffer, len);
   msgBuffer[len] = '\0';  // null terminate the buffer string just to make debug easier and reduce errors

   //DebugLog ( << "UDP Rcv : " << len << " b" );
   //DebugLog ( << Data(buffer, len).escaped().c_str());

   std::unique_ptr<SipMessage> message(new SipMessage(&mTuple));

   // set the received from information into the received= parameter in the
   // via

   // It is presumed that UDP Datagrams are arriving atomically and that
   // each one is a unique SIP message

   // Save all the info where this message came from
   message->setSource(sender);
   //DebugLog (<< "Received from: " << sender);

   // Tell the SipMessage about this datagram buffer.
   // WATCHOUT: below here buffer is consumed by message
   message->addBuffer(msgBuffer);

   mMsgHeaderScanner.prepareForMessage(message.get());

   char *unprocessedCharPtr;
   if (mMsgHeaderScanner.scanChunk(msgBuffer,
                                   len,
                                   &unprocessedCharPtr) !=
       MsgHeaderScanner::scrEnd)
   {
      StackLog(<<"Scanner rejecting datagram as unparsable / fragmented from " << sender);
      StackLog(<< Data(Data::Borrow, msgBuffer, len));
      if(mExternalUnknownDatagramHandler)
      {
         std::unique_ptr<Data> datagram(new Data(msgBuffer, len));
         (*mExternalUnknownDatagramHandler)(this, sender, std::move(datagram));
      }

      return;
   }

   // no pp error
   const int used = static_cast<int>(unprocessedCharPtr - msgBuffer);

   if (used < len)
   {
      // body is present .. add it up.
      // NB. The Sip Message uses an overlay (again)
      // for the body. It ALSO expects that the body
      // will be contiguous (of course).
      // it doesn't need a new buffer in UDP b/c there
      // will only be one datagram per buffer. (1:1 strict)

      message->setBody(msgBuffer + used, len - used);
      //DebugLog(<<"added " << len-used << " byte body");
   }

   // .bwc. basicCheck takes up substantial CPU. Don't bother doing it
   // if we're overloaded.
   CongestionManager::RejectionBehavior behavior=getRejectionBehaviorForIncoming();
   if (behavior==CongestionManager::REJECTING_NON_ESSENTIAL
         || (behavior==CongestionManager::REJECTING_NEW_WORK
            && message->isRequest()))
   {
      // .bwc. If this fifo is REJECTING_NEW_WORK, we will drop
      // requests but not responses ( ?bwc? is this right for ACK?). 
      // If we are REJECTING_NON_ESSENTIAL, 
      // we reject all incoming work, since losing something from the 
      // wire will not cause instability or leaks (see 
      // CongestionManager.hxx)

      // .bwc. This handles all appropriate checking for whether
      // this is a response or an ACK.
      std::unique_ptr<SendData> tryLater(make503(*message, getExpectedWaitForIncoming()));
      if (tryLater)
      {
         send(std::move(tryLater));
      }

      return;  // dropping message due to congestion
   }

   if (!basicCheck(*message))
   {
      // cannot use it, so, punt on it...
      // basicCheck queued any response required
      return;
   }

   stampReceived(message.get());

#ifdef USE_SIGCOMP
   if (mCompression.isEnabled() && sc)
   {
      const Via &via = message->header(h_Vias).front();
      if (message->isRequest())
      {
         // For requests, the compartment ID is read out of the
         // top via header field; if not present, we use the
         // TCP connection for identification purposes.
         if (via.exists(p_sigcompId))
         {
            Data compId = via.param(p_sigcompId);
            if(!compId.empty())
            {
               // .bwc. Crash was happening here. Why was there an empty sigcomp id?
               mSigcompStack->provideCompartmentId(sc, compId.data(), compId.size());
            }
         }
         else
         {
            mSigcompStack->provideCompartmentId(sc, this, sizeof(this));
         }
      }
      else
      {
         // For responses, the compartment ID is supposed to be
         // the same as the compartment ID of the request. We
         // *could* dig down into the transaction layer to try to
         // figure this out, but that's a royal pain, and a rather
         // severe layer violation. In practice, we're going to ferret
         // the ID out of the the Via header field, which is where we
         // squirreled it away when we sent this request in the first place.
         // !bwc! This probably shouldn't be going out over the wire.
         Data compId = via.param(p_branch).getSigcompCompartment();
         if(!compId.empty())
         {
            mSigcompStack->provideCompartmentId(sc, compId.data(), compId.size());
         }
      }
   }
#endif

   pushRxMsgUp(message.release());
   ++mRxTransactionCnt;
}

bool
UdpTransport::stunSendTest(const Tuple&  dest)
{
   bool changePort=false;
   bool changeIP=false;

   StunAtrString username;
   StunAtrString password;

   username.sizeValue = 0;
   password.sizeValue = 0;

   StunMessage req;
   stunBuildReqSimple(&req, username, changePort , changeIP , 1);

   // Replace the first 4 bytes of the header with the correct magic cookie required for RFC5389
   uint32_t* magicCookieField = (uint32_t*)req.msgHdr.id.octet;
   *magicCookieField = htonl(StunMagicCookie);

   char* buf = new char[STUN_MAX_MESSAGE_SIZE];
   int len = STUN_MAX_MESSAGE_SIZE;

   int rlen = stunEncodeMessage(req, buf, len, password, false);

   SendData* stunRequest = new SendData(dest, buf, rlen);
   mTxFifo.add(stunRequest);

   mStunResult = StunResultNoResponse;

   return true;
}

UdpTransport::StunResult
UdpTransport::stunResult(Tuple& mappedAddress)
{
   resip::Lock lock(myMutex);

   if (mStunResult == StunResultSuccess)
   {
      mappedAddress = mStunMappedAddress;
   }
   return mStunResult;
}

void
UdpTransport::setExternalUnknownDatagramHandler(ExternalUnknownDatagramHandler *handler)
{
   mExternalUnknownDatagramHandler = handler;
}

void
UdpTransport::setRcvBufLen(int buflen)
{
   setSocketRcvBufLen(mFd, buflen);
}

/* ====================================================================
 * The Vovida Software License, Version 1.0
 *
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * ====================================================================
 *
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 * vi: set shiftwidth=3 expandtab:
 */

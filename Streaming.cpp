#include <fstream>
#include <algorithm>
#include <cryptopp/dh.h>
#include <cryptopp/gzip.h>
#include "util.h"
#include "Log.h"
#include "RouterInfo.h"
#include "RouterContext.h"
#include "Tunnel.h"
#include "Timestamp.h"
#include "CryptoConst.h"
#include "Garlic.h"
#include "NetDb.h"
#include "Streaming.h"

namespace i2p
{
namespace stream
{
	Stream::Stream (boost::asio::io_service& service, StreamingDestination * local, 
		const i2p::data::LeaseSet& remote): m_Service (service), m_SendStreamID (0), 
		m_SequenceNumber (0), m_LastReceivedSequenceNumber (0), m_IsOpen (false),  
		m_IsOutgoing(true), m_LeaseSetUpdated (true), m_LocalDestination (local), 
		m_RemoteLeaseSet (&remote), m_ReceiveTimer (m_Service)
	{
		m_RecvStreamID = i2p::context.GetRandomNumberGenerator ().GenerateWord32 ();
		UpdateCurrentRemoteLease ();
	}	

	Stream::Stream (boost::asio::io_service& service, StreamingDestination * local):
		m_Service (service), m_SendStreamID (0), m_SequenceNumber (0), m_LastReceivedSequenceNumber (0), 
		m_IsOpen (true), m_IsOutgoing(true), m_LeaseSetUpdated (true), m_LocalDestination (local),
		m_RemoteLeaseSet (nullptr), m_ReceiveTimer (m_Service)
	{
		m_RecvStreamID = i2p::context.GetRandomNumberGenerator ().GenerateWord32 ();
	}

	Stream::~Stream ()
	{
		m_ReceiveTimer.cancel ();
		while (!m_ReceiveQueue.empty ())
		{
			auto packet = m_ReceiveQueue.front ();
			m_ReceiveQueue.pop ();
			delete packet;
		}		
		for (auto it: m_SavedPackets)
			delete it;
	}	
		
	void Stream::HandleNextPacket (Packet * packet)
	{
		if (!m_SendStreamID) 
			m_SendStreamID = packet->GetReceiveStreamID (); 	

		uint32_t receivedSeqn = packet->GetSeqn ();
		if (!receivedSeqn && !packet->IsSYN ())
		{
			// plain ack
			LogPrint ("Plain ACK received");
			delete packet;
			return;
		}

		LogPrint ("Received seqn=", receivedSeqn); 
		if (!receivedSeqn || receivedSeqn == m_LastReceivedSequenceNumber + 1)
		{			
			// we have received next in sequence message
			ProcessPacket (packet);

			// we should also try stored messages if any
			for (auto it = m_SavedPackets.begin (); it != m_SavedPackets.end ();)
			{			
				if ((*it)->GetSeqn () == m_LastReceivedSequenceNumber + 1)
				{
					Packet * savedPacket = *it;
					m_SavedPackets.erase (it++);

					ProcessPacket (savedPacket);
				}
				else
					break;
			}

			// send ack for last message
			if (m_IsOpen)
				SendQuickAck ();
		}	
		else 
		{	
			if (receivedSeqn <= m_LastReceivedSequenceNumber)
			{
				// we have received duplicate. Most likely our outbound tunnel is dead
				LogPrint ("Duplicate message ", receivedSeqn, " received");
				UpdateCurrentRemoteLease (); // pick another lease
				SendQuickAck (); // resend ack for previous message again
				delete packet; // packet dropped
			}	
			else
			{
				LogPrint ("Missing messages from ", m_LastReceivedSequenceNumber + 1, " to ", receivedSeqn - 1);
				// save message and wait for missing message again
				SavePacket (packet);
			}	
		}	
	}	

	void Stream::SavePacket (Packet * packet)
	{
		m_SavedPackets.insert (packet);
	}	

	void Stream::ProcessPacket (Packet * packet)
	{
		// process flags
		uint32_t receivedSeqn = packet->GetSeqn ();
		uint16_t flags = packet->GetFlags ();
		LogPrint ("Process seqn=", receivedSeqn, ", flags=", flags);
		
		const uint8_t * optionData = packet->GetOptionData ();
		if (flags & PACKET_FLAG_SYNCHRONIZE)
		{
			LogPrint ("Synchronize");
		}	

		if (flags & PACKET_FLAG_FROM_INCLUDED)
		{
			LogPrint ("From identity");
			optionData += m_RemoteIdentity.FromBuffer (optionData, packet->GetOptionSize ());
			if (m_RemoteLeaseSet)
			{
				if (m_RemoteIdentity.Hash () != m_RemoteLeaseSet->GetIdentHash ()) // check recieved identity
				{
					LogPrint ("Unexpected identity ",  m_RemoteIdentity.Hash ().ToBase64 (), " ", m_RemoteLeaseSet->GetIdentHash ().ToBase64 (), " expected");
					m_RemoteLeaseSet = nullptr; 
				}
			}
			else
				LogPrint ("Incoming stream from ", m_RemoteIdentity.Hash ().ToBase64 ());
			optionData += sizeof (i2p::data::Identity);
		}	

		if (flags & PACKET_FLAG_MAX_PACKET_SIZE_INCLUDED)
		{
			LogPrint ("Max packet size");
			optionData += 2;
		}	
		
		if (flags & PACKET_FLAG_SIGNATURE_INCLUDED)
		{
			LogPrint ("Signature");
			optionData += 40;
		}	

		packet->offset = packet->GetPayload () - packet->buf;
		if (packet->GetLength () > 0)
		{	
			m_ReceiveQueue.push (packet);
			m_ReceiveTimer.cancel ();
		}	
		else
			delete packet;
		
		m_LastReceivedSequenceNumber = receivedSeqn;

		if (flags & PACKET_FLAG_CLOSE)
		{
			LogPrint ("Closed");
			SendQuickAck (); // send ack for close explicitly?
			m_IsOpen = false;
		}
	}	
		
	size_t Stream::Send (const uint8_t * buf, size_t len, int timeout)
	{
		Packet * p = new Packet ();
		uint8_t * packet = p->GetBuffer ();
		// TODO: implement setters
		size_t size = 0;
		*(uint32_t *)(packet + size) = htobe32 (m_SendStreamID);
		size += 4; // sendStreamID
		*(uint32_t *)(packet + size) = htobe32 (m_RecvStreamID);
		size += 4; // receiveStreamID
		*(uint32_t *)(packet + size) = htobe32 (m_SequenceNumber++);
		size += 4; // sequenceNum
		*(uint32_t *)(packet + size) = 0; // TODO
		size += 4; // ack Through
		packet[size] = 0; 
		size++; // NACK count
		size++; // resend delay
		if (!m_IsOpen)
		{	
			//  initial packet
			m_IsOpen = true;
			*(uint16_t *)(packet + size) = htobe16 (PACKET_FLAG_SYNCHRONIZE | 
				PACKET_FLAG_FROM_INCLUDED | PACKET_FLAG_SIGNATURE_INCLUDED | 
				PACKET_FLAG_MAX_PACKET_SIZE_INCLUDED | PACKET_FLAG_NO_ACK);
			size += 2; // flags
			*(uint16_t *)(packet + size) = htobe16 (sizeof (i2p::data::Identity) + 40 + 2); // identity + signature + packet size
			size += 2; // options size
			memcpy (packet + size, &m_LocalDestination->GetIdentity (), sizeof (i2p::data::Identity)); 
			size += sizeof (i2p::data::Identity); // from
			*(uint16_t *)(packet + size) = htobe16 (STREAMING_MTU);
			size += 2; // max packet size
			uint8_t * signature = packet + size; // set it later
			memset (signature, 0, 40); // zeroes for now
			size += 40; // signature		
			memcpy (packet + size, buf, len); 
			size += len; // payload
			m_LocalDestination->Sign (packet, size, signature);
		}	
		else
		{
			// follow on packet
			*(uint16_t *)(packet + size) = 0;
			size += 2; // flags
			*(uint16_t *)(packet + size) = 0; // no options
			size += 2; // options size
			memcpy (packet + size, buf, len); 
			size += len; // payload
		}	
		p->len = size;
		m_Service.post (boost::bind (&Stream::SendPacket, this, p));
		
		return len;
	}	

		
	void Stream::SendQuickAck ()
	{
		uint8_t packet[MAX_PACKET_SIZE];
		size_t size = 0;
		*(uint32_t *)(packet + size) = htobe32 (m_SendStreamID);
		size += 4; // sendStreamID
		*(uint32_t *)(packet + size) = htobe32 (m_RecvStreamID);
		size += 4; // receiveStreamID
		*(uint32_t *)(packet + size) = 0; // this is plain Ack message
		size += 4; // sequenceNum
		*(uint32_t *)(packet + size) = htobe32 (m_LastReceivedSequenceNumber);
		size += 4; // ack Through
		packet[size] = 0; 
		size++; // NACK count
		size++; // resend delay
		*(uint16_t *)(packet + size) = 0; // nof flags set
		size += 2; // flags
		*(uint16_t *)(packet + size) = 0; // no options
		size += 2; // options size
		
		if (SendPacket (packet, size))
			LogPrint ("Quick Ack sent");
	}	

	void Stream::Close ()
	{
		if (m_IsOpen)
		{	
			m_IsOpen = false;
			Packet * p = new Packet ();
			uint8_t * packet = p->GetBuffer ();
			size_t size = 0;
			*(uint32_t *)(packet + size) = htobe32 (m_SendStreamID);
			size += 4; // sendStreamID
			*(uint32_t *)(packet + size) = htobe32 (m_RecvStreamID);
			size += 4; // receiveStreamID
			*(uint32_t *)(packet + size) = htobe32 (m_SequenceNumber++);
			size += 4; // sequenceNum
			*(uint32_t *)(packet + size) = htobe32 (m_LastReceivedSequenceNumber);
			size += 4; // ack Through
			packet[size] = 0; 
			size++; // NACK count
			size++; // resend delay
			*(uint16_t *)(packet + size) = htobe16 (PACKET_FLAG_CLOSE | PACKET_FLAG_SIGNATURE_INCLUDED);
			size += 2; // flags
			*(uint16_t *)(packet + size) = htobe16 (40); // 40 bytes signature
			size += 2; // options size
			uint8_t * signature = packet + size;
			memset (packet + size, 0, 40);
			size += 40; // signature
			m_LocalDestination->Sign (packet, size, signature);
			
			p->len = size;
			m_Service.post (boost::bind (&Stream::SendPacket, this, p));
			LogPrint ("FIN sent");
		}	
	}

	size_t Stream::ConcatenatePackets (uint8_t * buf, size_t len)
	{
		size_t pos = 0;
		while (pos < len && !m_ReceiveQueue.empty ())
		{
			Packet * packet = m_ReceiveQueue.front ();
			size_t l = std::min (packet->GetLength (), len - pos);
			memcpy (buf + pos, packet->GetBuffer (), l);
			pos += l;
			packet->offset += l;
			if (!packet->GetLength ())
			{
				m_ReceiveQueue.pop ();
				delete packet;
			}	
		}	
		return pos; 
	}

	bool Stream::SendPacket (Packet * packet)
	{
		if (packet)
		{	
			bool ret = SendPacket (packet->GetBuffer (), packet->GetLength ());
			delete packet;
			return ret;
		}	
		else
			return false;
	}	
	
	bool Stream::SendPacket (const uint8_t * buf, size_t len)
	{	
		if (!m_RemoteLeaseSet)
		{
			UpdateCurrentRemoteLease ();	
			if (!m_RemoteLeaseSet)
			{
				LogPrint ("Can't send packet. Missing remote LeaseSet");
				return false;
			}
		}

		I2NPMessage * leaseSet = nullptr;

		if (m_LeaseSetUpdated)
		{	
			leaseSet = m_LocalDestination->GetLeaseSetMsg ();
			m_LeaseSetUpdated = false;
		}	

		I2NPMessage * msg = i2p::garlic::routing.WrapMessage (*m_RemoteLeaseSet, 
			CreateDataMessage (this, buf, len), leaseSet);
		auto outboundTunnel = m_LocalDestination->GetTunnelPool ()->GetNextOutboundTunnel ();
		if (outboundTunnel)
		{
			auto ts = i2p::util::GetMillisecondsSinceEpoch ();
			if (ts >= m_CurrentRemoteLease.endDate)
				UpdateCurrentRemoteLease ();
			if (ts < m_CurrentRemoteLease.endDate)
			{	
				outboundTunnel->SendTunnelDataMsg (m_CurrentRemoteLease.tunnelGateway, m_CurrentRemoteLease.tunnelID, msg);
				return true;
			}	
			else
			{
				LogPrint ("All leases are expired");
				DeleteI2NPMessage (msg);
			}	
		}	
		else
		{	
			LogPrint ("No outbound tunnels in the pool");
			DeleteI2NPMessage (msg);
		}	
		return false;
	}

	void Stream::UpdateCurrentRemoteLease ()
	{
		if (!m_RemoteLeaseSet)
		{
			m_RemoteLeaseSet = i2p::data::netdb.FindLeaseSet (m_RemoteIdentity.Hash ());
			if (!m_RemoteLeaseSet)	
				LogPrint ("LeaseSet ", m_RemoteIdentity.Hash ().ToBase64 (), " not found");
		}
		if (m_RemoteLeaseSet)
		{
			auto leases = m_RemoteLeaseSet->GetNonExpiredLeases ();
			if (!leases.empty ())
			{	
				uint32_t i = i2p::context.GetRandomNumberGenerator ().GenerateWord32 (0, leases.size () - 1);
				m_CurrentRemoteLease = leases[i];
			}	
			else
				m_CurrentRemoteLease.endDate = 0;
		}
		else
			m_CurrentRemoteLease.endDate = 0;
	}	
		

	StreamingDestination::StreamingDestination (boost::asio::io_service& service): 
		m_Service (service), m_LeaseSet (nullptr)
	{		
		m_Keys = i2p::data::CreateRandomKeys ();

		m_IdentHash = m_Keys.pub.Hash ();
		m_SigningPrivateKey.Initialize (i2p::crypto::dsap, i2p::crypto::dsaq, i2p::crypto::dsag, 
			CryptoPP::Integer (m_Keys.signingPrivateKey, 20));
		CryptoPP::DH dh (i2p::crypto::elgp, i2p::crypto::elgg);
		dh.GenerateKeyPair(i2p::context.GetRandomNumberGenerator (), m_EncryptionPrivateKey, m_EncryptionPublicKey);
		m_Pool = i2p::tunnel::tunnels.CreateTunnelPool (*this, 3); // 3-hops tunnel
	}

	StreamingDestination::StreamingDestination (boost::asio::io_service& service, const std::string& fullPath):
		m_Service (service), m_LeaseSet (nullptr) 
	{
		std::ifstream s(fullPath.c_str (), std::ifstream::binary);
		if (s.is_open ())	
			s.read ((char *)&m_Keys, sizeof (m_Keys));
		else
			LogPrint ("Can't open file ", fullPath);

		m_IdentHash = m_Keys.pub.Hash ();
		m_SigningPrivateKey.Initialize (i2p::crypto::dsap, i2p::crypto::dsaq, i2p::crypto::dsag, 
			CryptoPP::Integer (m_Keys.signingPrivateKey, 20));
		CryptoPP::DH dh (i2p::crypto::elgp, i2p::crypto::elgg);
		dh.GenerateKeyPair(i2p::context.GetRandomNumberGenerator (), m_EncryptionPrivateKey, m_EncryptionPublicKey);
		m_Pool = i2p::tunnel::tunnels.CreateTunnelPool (*this, 3); // 3-hops tunnel 
	}

	StreamingDestination::~StreamingDestination ()
	{
		if (m_Pool)
			i2p::tunnel::tunnels.DeleteTunnelPool (m_Pool);		
		delete m_LeaseSet;
	}	
	
	void StreamingDestination::HandleNextPacket (Packet * packet)
	{
		uint32_t sendStreamID = packet->GetSendStreamID ();
		if (sendStreamID)
		{	
			auto it = m_Streams.find (sendStreamID);
			if (it != m_Streams.end ())
				it->second->HandleNextPacket (packet);
			else
			{	
				LogPrint ("Unknown stream ", sendStreamID);
				delete packet;
			}
		}	
		else // new incoming stream
		{
			auto incomingStream = CreateNewIncomingStream ();
			if (m_Acceptor != nullptr)
				m_Acceptor (incomingStream);
			incomingStream->HandleNextPacket (packet);
		}	
	}	

	Stream * StreamingDestination::CreateNewOutgoingStream (const i2p::data::LeaseSet& remote)
	{
		Stream * s = new Stream (m_Service, this, remote);
		m_Streams[s->GetRecvStreamID ()] = s;
		return s;
	}	

	Stream * StreamingDestination::CreateNewIncomingStream ()
	{
		Stream * s = new Stream (m_Service, this);
		m_Streams[s->GetRecvStreamID ()] = s;
		return s;
	}

	void StreamingDestination::DeleteStream (Stream * stream)
	{
		if (stream)
		{
			m_Streams.erase (stream->GetRecvStreamID ());
			delete stream;
		}	
	}	
		
	I2NPMessage * StreamingDestination::GetLeaseSetMsg ()
	{		
		return CreateDatabaseStoreMsg (GetLeaseSet ());
	}	

	const i2p::data::LeaseSet * StreamingDestination::GetLeaseSet ()
	{
		if (!m_Pool) return nullptr;
		if (!m_LeaseSet || m_LeaseSet->HasExpiredLeases ())
		{	
			auto newLeaseSet = new i2p::data::LeaseSet (*m_Pool);
			// TODO: make it atomic
			auto oldLeaseSet = m_LeaseSet;
			m_LeaseSet = newLeaseSet;
			delete oldLeaseSet;
			for (auto it: m_Streams)
				it.second->SetLeaseSetUpdated ();
		}	
		return m_LeaseSet;
	}	
		
	void StreamingDestination::Sign (const uint8_t * buf, int len, uint8_t * signature) const
	{
		CryptoPP::DSA::Signer signer (m_SigningPrivateKey);
		signer.SignMessage (i2p::context.GetRandomNumberGenerator (), buf, len, signature);
	}

	StreamingDestinations destinations;	
	void StreamingDestinations::Start ()
	{
		if (!m_SharedLocalDestination)
		{	
			m_SharedLocalDestination = new StreamingDestination (m_Service);
			m_Destinations[m_SharedLocalDestination->GetIdentHash ()] = m_SharedLocalDestination;
		}
		LoadLocalDestinations ();	
		
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&StreamingDestinations::Run, this));
	}
		
	void StreamingDestinations::Stop ()
	{
		for (auto it: m_Destinations)
			delete it.second;	
		m_Destinations.clear ();
		m_SharedLocalDestination = 0; // deleted through m_Destination
		
		m_IsRunning = false;
		m_Service.stop ();
		if (m_Thread)
		{	
			m_Thread->join (); 
			delete m_Thread;
			m_Thread = 0;
		}	
	}	
		
	void StreamingDestinations::Run ()
	{
		m_Service.run ();
	}	

	void StreamingDestinations::LoadLocalDestinations ()
	{
		int numDestinations = 0;
		boost::filesystem::path p (i2p::util::filesystem::GetDataDir());
		boost::filesystem::directory_iterator end;
		for (boost::filesystem::directory_iterator it (p); it != end; ++it)
		{
			if (boost::filesystem::is_regular_file (*it) && it->path ().extension () == ".dat")
			{
				auto fullPath =
#if BOOST_VERSION > 10500
				it->path().string();
#else
				it->path();
#endif
				auto localDestination = new StreamingDestination (m_Service, fullPath);
				m_Destinations[localDestination->GetIdentHash ()] = localDestination;
				numDestinations++;
			}	
		}	
		if (numDestinations > 0)
			LogPrint (numDestinations, " local destinations loaded");
	}	
	
	Stream * StreamingDestinations::CreateClientStream (const i2p::data::LeaseSet& remote)
	{
		if (!m_SharedLocalDestination) return nullptr;
		return m_SharedLocalDestination->CreateNewOutgoingStream (remote);
	}

	void StreamingDestinations::DeleteStream (Stream * stream)
	{
		if (stream)
		{	
			m_Service.post (
				[=](void)
				{
					stream->GetLocalDestination ()->DeleteStream (stream);
				}	
				            );
		}	
	}	
		
	void StreamingDestinations::HandleNextPacket (i2p::data::IdentHash destination, Packet * packet)
	{
		m_Service.post (boost::bind (&StreamingDestinations::PostNextPacket, this, destination, packet)); 
	}	
	
	void StreamingDestinations::PostNextPacket (i2p::data::IdentHash destination, Packet * packet)
	{
		auto it = m_Destinations.find (destination);
		if (it != m_Destinations.end ())
			it->second->HandleNextPacket (packet);
		else
		{
			LogPrint ("Local destination ", destination.ToBase64 (), " not found");
			delete packet;
		}
	}	
		
	Stream * CreateStream (const i2p::data::LeaseSet& remote)
	{
		return destinations.CreateClientStream (remote);
	}
		
	void DeleteStream (Stream * stream)
	{
		destinations.DeleteStream (stream);
	}	

	void StartStreaming ()
	{
		destinations.Start ();
	}
		
	void StopStreaming ()
	{
		destinations.Stop ();
	}	

	StreamingDestination * GetSharedLocalDestination ()
	{
		return destinations.GetSharedLocalDestination ();
	}	
		
	void HandleDataMessage (i2p::data::IdentHash destination, const uint8_t * buf, size_t len)
	{
		uint32_t length = be32toh (*(uint32_t *)buf);
		buf += 4;
		// we assume I2CP payload
		if (buf[9] == 6) // streaming protocol
		{	
			// unzip it
			CryptoPP::Gunzip decompressor;
			decompressor.Put (buf, length);
			decompressor.MessageEnd();
			Packet * uncompressed = new Packet;
			uncompressed->offset = 0;
			uncompressed->len = decompressor.MaxRetrievable ();
			if (uncompressed->len > MAX_PACKET_SIZE)
			{
				LogPrint ("Recieved packet size exceeds mac packet size");
				uncompressed->len = MAX_PACKET_SIZE;
			}	
			decompressor.Get (uncompressed->buf, uncompressed->len);
			// then forward to streaming engine thread
			destinations.HandleNextPacket (destination, uncompressed);
		}	
		else
			LogPrint ("Data: protocol ", buf[9], " is not supported");
	}	

	I2NPMessage * CreateDataMessage (Stream * s, const uint8_t * payload, size_t len)
	{
		I2NPMessage * msg = NewI2NPMessage ();
		CryptoPP::Gzip compressor;
		compressor.SetDeflateLevel (CryptoPP::Gzip::MIN_DEFLATE_LEVEL);
		compressor.Put (payload, len);
		compressor.MessageEnd();
		int size = compressor.MaxRetrievable ();
		uint8_t * buf = msg->GetPayload ();
		*(uint32_t *)buf = htobe32 (size); // length
		buf += 4;
		compressor.Get (buf, size);
		memset (buf + 4, 0, 4); // source and destination ports. TODO: fill with proper values later
		buf[9] = 6; // streaming protocol
		msg->len += size + 4; 
		FillI2NPMessageHeader (msg, eI2NPData);
		
		return msg;
	}	
}		
}	

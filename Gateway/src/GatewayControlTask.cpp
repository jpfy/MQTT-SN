/*
 * GatewayControlTask.cpp
 *
 *                      The BSD License
 *
 *           Copyright (c) 2014, tomoaki@tomy-tech.com
 *                    All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  Created on: 2014/06/01
 *    Modified:
 *      Author: Tomoaki YAMAGUCHI
 *     Version: 0.0.0
 */

#include <stdio.h>
#include <iostream>
#include <string.h>
#include <time.h>
#include "GatewayControlTask.h"
#include "lib/ProcessFramework.h"
#include "GatewayResourcesProvider.h"
#include "GatewayDefines.h"
#include "lib/Messages.h"
#include "lib/ErrorMessage.h"

extern char* currentDateTime();
extern uint16_t getUint16(uint8_t* pos);
extern void setUint32(uint8_t* pos, uint32_t val);

/*=====================================
        Class GatewayControlTask
 =====================================*/

GatewayControlTask::GatewayControlTask(GatewayResourcesProvider* res){
	_res = res;
	_res->attach(this);
	_eventQue = 0;
	_protocol = MQTT_PROTOCOL_VER4;
	_loginId = "";
	_password = "";
}

GatewayControlTask::~GatewayControlTask(){

}


void GatewayControlTask::run(){
	Timer advertiseTimer;
	Timer sendUnixTimer;
	Event* ev = 0;

	_gatewayId = atoi(_res->getArgv('i'));
	if (_gatewayId == 0 || _gatewayId > 255){
		THROW_EXCEPTION(ExFatal, ERRNO_SYS_05, "Invalid Gateway Id");  // ABORT
	}

	int keepAlive = KEEP_ALIVE_TIME;
	if(_res->getArgv('k') != 0){
		keepAlive =atoi( _res->getArgv('k'));
	}

	if(_res->getArgv('l') != 0){
		_loginId = _res->getArgv('l');
	}
	if(_res->getArgv('w') != 0){
		_password = _res->getArgv('w');
	}

	_eventQue = _res->getGatewayEventQue();

	advertiseTimer.start(keepAlive*1000UL);

	printf("%s TomyGateway start\n", currentDateTime());


	while(true){
		ev = _eventQue->timedwait(TIMEOUT_PERIOD);

		/*------     Check Client is Lost    ---------*/
		if(ev->getEventType() == EtTimeout){
			ClientList* clist = _res->getClientList();

			for( int i = 0; i < clist->getClientCount(); i++){
				if((*clist)[i]){
					(*clist)[i]->checkTimeover();
				}else{
					break;
				}
			}

			/*------ Check Keep Alive Timer & send Advertise ------*/
			if(advertiseTimer.isTimeup()){
				MQTTSnAdvertise* adv = new MQTTSnAdvertise();
				adv->setGwId(_gatewayId);
				adv->setDuration(keepAlive);
				Event* ev1 = new Event();
				ev1->setEvent(adv);  //broadcast
				printf(YELLOW_FORMAT2, currentDateTime(), "ADVERTISE", LEFTARROW, GATEWAY, msgPrint(adv));

				_res->getClientSendQue()->post(ev1);
				advertiseTimer.start(keepAlive*1000UL);
				sendUnixTimer.start(SEND_UNIXTIME_TIME * 1000UL);
			}

			/*------ Check Timer & send UixTime ------*/
			if(sendUnixTimer.isTimeup()){
				uint8_t buf[4];
				uint32_t tm = time(0);
				setUint32(buf,tm);

				MQTTSnPublish* msg = new MQTTSnPublish();

				msg->setTopicId(MQTTSN_TOPICID_PREDEFINED_TIME);
				msg->setTopicIdType(MQTTSN_TOPIC_TYPE_PREDEFINED);
				msg->setData(buf, 4);
				msg->setQos(0);

				Event* ev1 = new Event();
				ev1->setEvent(msg);
				printf(YELLOW_FORMAT2, currentDateTime(), "PUBLISH", LEFTARROW, GATEWAY, msgPrint(msg));

				_res->getClientSendQue()->post(ev1);
				sendUnixTimer.stop();
			}
		}

		/*------   Check  SEARCHGW & send GWINFO      ---------*/
		else if(ev->getEventType() == EtBroadcast){
			MQTTSnMessage* msg = ev->getMqttSnMessage();
			printf(YELLOW_FORMAT2, currentDateTime(), "SERCHGW", LEFTARROW, CLIENT, msgPrint(msg));

			if(msg->getType() == MQTTSN_TYPE_SEARCHGW){
				if(_res->getClientList()->getClientCount() <  MAX_CLIENT_NODES ){
					MQTTSnGwInfo* gwinfo = new MQTTSnGwInfo();
					gwinfo->setGwId(_gatewayId);
					Event* ev1 = new Event();
					ev1->setEvent(gwinfo);
					printf(YELLOW_FORMAT1, currentDateTime(), "GWINFO", RIGHTARROW, CLIENT, msgPrint(gwinfo));

					_res->getClientSendQue()->post(ev1);
				}
			}

		}
		
		/*------   Message form Clients      ---------*/
		else if(ev->getEventType() == EtClientRecv){
			printf("Received message from client!");
			ClientNode* clnode = ev->getClientNode();
			MQTTSnMessage* msg = clnode->getClientRecvMessage();
			
			if(msg->getType() == MQTTSN_TYPE_PUBLISH){
				handleSnPublish(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_SUBSCRIBE){
				handleSnSubscribe(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_UNSUBSCRIBE){
				handleSnUnsubscribe(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_PINGREQ){
				handleSnPingReq(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_PUBACK){
				handleSnPubAck(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_CONNECT){
				handleSnConnect(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_WILLTOPIC){
				handleSnWillTopic(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_WILLMSG){
				handleSnWillMsg(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_DISCONNECT){
				handleSnDisconnect(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_REGISTER){
				handleSnRegister(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_PUBREC){
				handleSnPubRec(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_PUBREL){
				handleSnPubRel(ev, clnode, msg);
			}else if(msg->getType() == MQTTSN_TYPE_PUBCOMP){
				handleSnPubComp(ev, clnode, msg);
			}else{
				printf("%s   Irregular ClientRecvMessage\n", currentDateTime());
			}

		}
		/*------   Message form Broker      ---------*/
		else if(ev->getEventType() == EtBrokerRecv){

			ClientNode* clnode = ev->getClientNode();
			MQTTMessage* msg = clnode->getBrokerRecvMessage();
			
			if(msg->getType() == MQTT_TYPE_PUBACK){
				handlePuback(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_PINGRESP){
				handlePingresp(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_SUBACK){
				handleSuback(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_UNSUBACK){
				handleUnsuback(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_CONNACK){
				handleConnack(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_PUBLISH){
				handlePublish(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_DISCONNECT){
				handleDisconnect(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_PUBREC){
				handlePubRec(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_PUBREL){
				handlePubRel(ev, clnode, msg);
			}else if(msg->getType() == MQTT_TYPE_PUBCOMP){
				handlePubComp(ev, clnode, msg);
			}else{
				printf("%s   Irregular BrokerRecvMessage\n", currentDateTime());
			}
		}

		delete ev;
	}
}

/*=======================================================
                     Upstream
 ========================================================*/

/*-------------------------------------------------------
 *               Upstream MQTTSnPublish
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPublish(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(BLUE_FORMAT2, currentDateTime(), "PUBLISH", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnPublish* sPublish = new MQTTSnPublish();
	MQTTPublish* mqMsg = new MQTTPublish();
	sPublish->absorb(msg);

	Topic* tp = clnode->getTopics()->getTopic(sPublish->getTopicId());

	if(tp || ((sPublish->getFlags() && MQTTSN_TOPIC_TYPE) == MQTTSN_TOPIC_TYPE_SHORT)){
		if(tp){
			mqMsg->setTopic(tp->getTopicName());
		}else{
			string str;
			mqMsg->setTopic(sPublish->getTopic(&str));
		}
		if(sPublish->getMsgId()){
			MQTTSnPubAck* sPuback = new MQTTSnPubAck();
			sPuback->setMsgId(sPublish->getMsgId());
			sPuback->setTopicId(sPublish->getTopicId());
			if(clnode->getWaitedPubAck()){
				delete clnode->getWaitedPubAck();
			}
			clnode->setWaitedPubAck(sPuback);

			mqMsg->setMessageId(sPublish->getMsgId());
		}

		mqMsg->setQos(sPublish->getQos());

		if(sPublish->getFlags() && MQTTSN_FLAG_DUP){
			mqMsg->setDup();
		}
		if(sPublish->getFlags() && MQTTSN_FLAG_RETAIN){
			mqMsg->setRetain();
		}

		mqMsg->setPayload(sPublish->getData() , sPublish->getDataLength());

		clnode->setBrokerSendMessage(mqMsg);
		Event* ev1 = new Event();
		ev1->setBrokerSendEvent(clnode);
		_res->getBrokerSendQue()->post(ev1);

	}else{
		if(sPublish->getMsgId()){
			MQTTSnPubAck* sPuback = new MQTTSnPubAck();
			sPuback->setMsgId(sPublish->getMsgId());
			sPuback->setTopicId(sPublish->getTopicId());
			sPuback->setReturnCode(MQTTSN_RC_REJECTED_INVALID_TOPIC_ID);

			clnode->setClientSendMessage(sPuback);

			Event* ev1 = new Event();
			ev1->setClientSendEvent(clnode);
			printf(BLUE_FORMAT1, currentDateTime(), "PUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(sPuback));

			_res->getClientSendQue()->post(ev1);  // Send PubAck INVALID_TOPIC_ID
		}
	}
	delete sPublish;
}

/*-------------------------------------------------------
                Upstream MQTTSnSubscribe
 -------------------------------------------------------*/
void GatewayControlTask::handleSnSubscribe(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){
	printf(FORMAT2, currentDateTime(), "SUBSCRIBE", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnSubscribe* sSubscribe = new MQTTSnSubscribe();
	MQTTSubscribe* subscribe = new MQTTSubscribe();
	sSubscribe->absorb(msg);

	uint8_t topicIdType = sSubscribe->getFlags() & 0x03;

	subscribe->setMessageId(sSubscribe->getMsgId());

	if(sSubscribe->getFlags() & MQTTSN_FLAG_DUP ){
		subscribe->setDup();
	}
	if(sSubscribe->getFlags() & MQTTSN_FLAG_RETAIN){
		subscribe->setRetain();
	}
	subscribe->setQos(sSubscribe->getQos());

	if(topicIdType != MQTTSN_FLAG_TOPICID_TYPE_RESV){
		if(topicIdType == MQTTSN_FLAG_TOPICID_TYPE_PREDEFINED){
			/*----- Predefined TopicId ------*/
			MQTTSnSubAck* sSuback = new MQTTSnSubAck();

			if(sSubscribe->getMsgId()){       // MessageID
				sSuback->setQos(sSubscribe->getQos());
				sSuback->setTopicId(sSubscribe->getTopicId());
				sSuback->setMsgId(sSubscribe->getMsgId());

				if(sSubscribe->getTopicId() == MQTTSN_TOPICID_PREDEFINED_TIME){
					sSuback->setReturnCode(MQTT_RC_ACCEPTED);
				}else{
					sSuback->setReturnCode(MQTT_RC_REFUSED_IDENTIFIER_REJECTED);
				}

				clnode->setClientSendMessage(sSuback);

				Event* evsuback = new Event();
				evsuback->setClientSendEvent(clnode);
				printf(FORMAT1, currentDateTime(), "SUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(sSuback));

				_res->getClientSendQue()->post(evsuback);
			}

			if(sSubscribe->getTopicId() == MQTTSN_TOPICID_PREDEFINED_TIME){

				MQTTSnPublish* pub = new MQTTSnPublish();
				pub->setTopicIdType(1);  // pre-defined
				pub->setTopicId(MQTTSN_TOPICID_PREDEFINED_TIME);
				pub->setMsgId(clnode->getNextSnMsgId());

				uint8_t buf[4];
				uint32_t tm = time(0);
				setUint32(buf,tm);

				pub->setData(buf, 4);
				printf(GREEN_FORMAT1, currentDateTime(), "PUBLISH", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(pub));

				clnode->setClientSendMessage(pub);

				Event *evpub = new Event();
				evpub->setClientSendEvent(clnode);
				_res->getClientSendQue()->post(evpub);
			}
			delete subscribe;
		}else{
			uint16_t tpId;

			Topic* tp = clnode->getTopics()->getTopic(sSubscribe->getTopicName());
			if (tp){
				tpId = tp->getTopicId();
			}else{
				tpId = clnode->getTopics()->createTopic(sSubscribe->getTopicName());
			}

			subscribe->setTopic(sSubscribe->getTopicName(), sSubscribe->getQos());
			if(sSubscribe->getMsgId()){
				MQTTSnSubAck* sSuback = new MQTTSnSubAck();
				sSuback->setMsgId(sSubscribe->getMsgId());
				sSuback->setTopicId(tpId);
				clnode->setWaitedSubAck(sSuback);
			}

			clnode->setBrokerSendMessage(subscribe);
			Event* ev1 = new Event();
			ev1->setBrokerSendEvent(clnode);
			_res->getBrokerSendQue()->post(ev1);
			delete sSubscribe;
			return;
		}

	}else{
		/*-- Irregular TopicIdType --*/
		if(sSubscribe->getMsgId()){
			MQTTSnSubAck* sSuback = new MQTTSnSubAck();
			sSuback->setMsgId(sSubscribe->getMsgId());
			sSuback->setTopicId(sSubscribe->getTopicId());
			sSuback->setReturnCode(MQTT_RC_REFUSED_IDENTIFIER_REJECTED);

			clnode->setClientSendMessage(sSuback);

			Event* evun = new Event();
			evun->setClientSendEvent(clnode);
			printf(FORMAT1, currentDateTime(), "SUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(sSuback));

			_res->getClientSendQue()->post(evun);  // Send SUBACK to Client
		}
		delete subscribe;
	}
	delete sSubscribe;
}

/*-------------------------------------------------------
                Upstream MQTTSnUnsubscribe
 -------------------------------------------------------*/
void GatewayControlTask::handleSnUnsubscribe(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT2, currentDateTime(), "UNSUBSCRIBE", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnUnsubscribe* sUnsubscribe = new MQTTSnUnsubscribe();
	MQTTUnsubscribe* unsubscribe = new MQTTUnsubscribe();
	sUnsubscribe->absorb(msg);

	uint8_t topicIdType = sUnsubscribe->getFlags() & 0x03;

	unsubscribe->setMessageId(sUnsubscribe->getMsgId());

	if(topicIdType != MQTTSN_FLAG_TOPICID_TYPE_RESV){

		if(topicIdType == MQTTSN_FLAG_TOPICID_TYPE_SHORT){
			unsubscribe->setTopicName(sUnsubscribe->getTopicName()); // TopicName

		}else if(clnode->getTopics()->getTopic(sUnsubscribe->getTopicId())){

			if(topicIdType == MQTTSN_FLAG_TOPICID_TYPE_PREDEFINED) goto uslbl1;

			unsubscribe->setTopicName(sUnsubscribe->getTopicName());
		}

		clnode->setBrokerSendMessage(unsubscribe);

		Event* ev1 = new Event();
		ev1->setBrokerSendEvent(clnode);
		_res->getBrokerSendQue()->post(ev1);    //  UNSUBSCRIBE to Broker
		delete sUnsubscribe;
		return;
	}

	/*-- Irregular TopicIdType or MQTTSN_FLAG_TOPICID_TYPE_PREDEFINED --*/
uslbl1:
	if(sUnsubscribe->getMsgId()){
		MQTTSnUnsubAck* sUnsuback = new MQTTSnUnsubAck();
		sUnsuback->setMsgId(sUnsubscribe->getMsgId());

		clnode->setClientSendMessage(sUnsuback);

		Event* evun = new Event();
		evun->setClientSendEvent(clnode);
		printf(FORMAT1, currentDateTime(), "UNSUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(sUnsuback));

		_res->getClientSendQue()->post(evun);  // Send UNSUBACK to Client
	}

	delete sUnsubscribe;
}

/*-------------------------------------------------------
                Upstream MQTTSnPingReq
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPingReq(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT2, currentDateTime(), "PINGREQ", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTPingReq* pingReq = new MQTTPingReq();

	clnode->setBrokerSendMessage(pingReq);

	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Upstream MQTTSnPubAck
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPubAck(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(GREEN_FORMAT1, currentDateTime(), "PUBACK", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnPubAck* sPubAck = new MQTTSnPubAck();
	MQTTPubAck* pubAck = new MQTTPubAck();
	sPubAck->absorb(msg);

	pubAck->setMessageId(sPubAck->getMsgId());

	clnode->setBrokerSendMessage(pubAck);
	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);
	delete sPubAck;
}

/*-------------------------------------------------------
                Upstream MQTTSnPubRec
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPubRec(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(GREEN_FORMAT1, currentDateTime(), "PUBREC", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnPubRec* sPubRec = new MQTTSnPubRec();
	MQTTPubRec* pubRec = new MQTTPubRec();
	sPubRec->absorb(msg);

	pubRec->setMessageId(sPubRec->getMsgId());

	clnode->setBrokerSendMessage(pubRec);
	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);
	delete sPubRec;
}

/*-------------------------------------------------------
                Upstream MQTTSnPubRel
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPubRel(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(GREEN_FORMAT1, currentDateTime(), "PUBREL", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnPubRel* sPubRel = new MQTTSnPubRel();
	MQTTPubRel* pubRel = new MQTTPubRel();
	sPubRel->absorb(msg);

	pubRel->setMessageId(sPubRel->getMsgId());

	clnode->setBrokerSendMessage(pubRel);
	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);
	delete sPubRel;
}

/*-------------------------------------------------------
                Upstream MQTTSnPubComp
 -------------------------------------------------------*/
void GatewayControlTask::handleSnPubComp(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(GREEN_FORMAT1, currentDateTime(), "PUBREL", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnPubComp* sPubComp= new MQTTSnPubComp();
	MQTTPubComp* pubComp = new MQTTPubComp();
	sPubComp->absorb(msg);

	pubComp->setMessageId(sPubComp->getMsgId());

	clnode->setBrokerSendMessage(pubComp);
	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);
	delete sPubComp;
}

/*-------------------------------------------------------
                Upstream MQTTSnConnect
 -------------------------------------------------------*/
void GatewayControlTask::handleSnConnect(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT2, currentDateTime(), "CONNECT", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	Topics* topics = clnode->getTopics();
	MQTTSnConnect* sConnect = new MQTTSnConnect();
	MQTTConnect* mqMsg = new MQTTConnect();
	mqMsg->setProtocol(_protocol);
	sConnect->absorb(msg);

	mqMsg->setClientId(clnode->getNodeId());
	mqMsg->setKeepAliveTime(sConnect->getDuration());

	if(_loginId != "" && _password != ""){
		mqMsg->setUserName(&_loginId);
		mqMsg->setPassword(&_password);
	}
	clnode->setConnectMessage(mqMsg);

	if(sConnect->isCleanSession()){
		if(topics){
			delete topics;
		}
		topics = new Topics();
		clnode->setTopics(topics);
		mqMsg->setCleanSessionFlg();
	}

	if(sConnect->isWillRequired()){
		MQTTSnWillTopicReq* reqTopic = new MQTTSnWillTopicReq();

		Event* evwr = new Event();

		clnode->setClientSendMessage(reqTopic);
		evwr->setClientSendEvent(clnode);
		printf(FORMAT1, currentDateTime(), "WILLTOPICREQ", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(reqTopic));

		_res->getClientSendQue()->post(evwr);  // Send WILLTOPICREQ to Client
	}else{
		Event* ev1 = new Event();
		clnode->setBrokerSendMessage(clnode->getConnectMessage());
		ev1->setBrokerSendEvent(clnode);
		_res->getBrokerSendQue()->post(ev1);
	}
	delete sConnect;
}

/*-------------------------------------------------------
                Upstream MQTTSnWillTopic
 -------------------------------------------------------*/
void GatewayControlTask::handleSnWillTopic(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT1, currentDateTime(), "WILLTOPIC", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnWillTopic* snMsg = new MQTTSnWillTopic();
	MQTTSnWillMsgReq* reqMsg = new MQTTSnWillMsgReq();
	snMsg->absorb(msg);

	if(clnode->getConnectMessage()){
		clnode->getConnectMessage()->setWillTopic(snMsg->getWillTopic());
		clnode->getConnectMessage()->setWillQos(snMsg->getQos());

		clnode->setClientSendMessage(reqMsg);

		Event* evt = new Event();
		evt->setClientSendEvent(clnode);
		printf(FORMAT1, currentDateTime(), "WILLMSGREQ", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(reqMsg));

		_res->getClientSendQue()->post(evt);  // Send WILLMSGREQ to Client
	}
	delete snMsg;
}

/*-------------------------------------------------------
                Upstream MQTTSnWillMsg
 -------------------------------------------------------*/
void GatewayControlTask::handleSnWillMsg(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT1, currentDateTime(), "WILLMSG", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnWillMsg* snMsg = new MQTTSnWillMsg();
	snMsg->absorb(msg);

	if(clnode->getConnectMessage()){
		clnode->getConnectMessage()->setWillMessage(snMsg->getWillMsg());

		clnode->setBrokerSendMessage(clnode->getConnectMessage());
		clnode->setConnectMessage(0);

		Event* ev1 = new Event();
		ev1->setBrokerSendEvent(clnode);
		_res->getBrokerSendQue()->post(ev1);

	}else{
		MQTTSnConnack* connack = new MQTTSnConnack();
		connack->setReturnCode(MQTTSN_RC_REJECTED_CONGESTION);

		clnode->setClientSendMessage(connack);
		Event* ev1 = new Event();
		ev1->setClientSendEvent(clnode);
		printf(RED_FORMAT1, currentDateTime(), "*CONNACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(connack));

		_res->getClientSendQue()->post(ev1);  // Send CONNACK REJECTED CONGESTION to Client

	}
	delete snMsg;
}

/*-------------------------------------------------------
                Upstream MQTTSnDisconnect
 -------------------------------------------------------*/
void GatewayControlTask::handleSnDisconnect(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT2, currentDateTime(), "DISCONNECT", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnDisconnect* snMsg = new MQTTSnDisconnect();
	MQTTDisconnect* mqMsg = new MQTTDisconnect();
	snMsg->absorb(msg);

	clnode->setBrokerSendMessage(mqMsg);

	Event* ev1 = new Event();
	ev1->setBrokerSendEvent(clnode);
	_res->getBrokerSendQue()->post(ev1);

	delete snMsg;
}


/*-------------------------------------------------------
                Upstream MQTTSnRegister
 -------------------------------------------------------*/
void GatewayControlTask::handleSnRegister(Event* ev, ClientNode* clnode, MQTTSnMessage* msg){

	printf(FORMAT2, currentDateTime(), "REGISTER", LEFTARROW, clnode->getNodeId()->c_str(), msgPrint(msg));

	MQTTSnRegister* snMsg = new MQTTSnRegister();
	MQTTSnRegAck* respMsg = new MQTTSnRegAck();
	snMsg->absorb(msg);

	respMsg->setMsgId(snMsg->getMsgId());

	uint16_t tpId = clnode->getTopics()->createTopic(snMsg->getTopicName());

	respMsg->setTopicId(tpId);
	respMsg->setReturnCode(MQTTSN_RC_ACCEPTED);

	clnode->setClientSendMessage(respMsg);

	Event* evrg = new Event();
	evrg->setClientSendEvent(clnode);
	printf(FORMAT1, currentDateTime(), "REGACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(respMsg));

	_res->getClientSendQue()->post(evrg);

	delete snMsg;
}
	

/*=======================================================
                     Downstream
 ========================================================*/

/*-------------------------------------------------------
                Downstream MQTTPubAck
 -------------------------------------------------------*/
void GatewayControlTask::handlePuback(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTPubAck* mqMsg = static_cast<MQTTPubAck*>(msg);
	MQTTSnPubAck* snMsg = clnode->getWaitedPubAck();

	if(snMsg){
		printf(BLUE_FORMAT1, currentDateTime(), "PUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

		if(snMsg->getMsgId() == mqMsg->getMessageId()){
			clnode->setWaitedPubAck(0);
			clnode->setClientSendMessage(snMsg);
			Event* ev1 = new Event();
			ev1->setClientSendEvent(clnode);
			_res->getClientSendQue()->post(ev1);
		}
	}
}

/*-------------------------------------------------------
                Downstream MQTTPubRec
 -------------------------------------------------------*/
void GatewayControlTask::handlePubRec(Event* ev, ClientNode* clnode, MQTTMessage* msg){
	MQTTSnPubRec* snMsg = new MQTTSnPubRec();
	MQTTPubRec* mqMsg = static_cast<MQTTPubRec*>(msg);
	snMsg->setMsgId(mqMsg->getMessageId());
	printf(BLUE_FORMAT1, currentDateTime(), "PUBREC", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));
	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Downstream MQTTPubRel
 -------------------------------------------------------*/
void GatewayControlTask::handlePubRel(Event* ev, ClientNode* clnode, MQTTMessage* msg){
	MQTTSnPubRel* snMsg = new MQTTSnPubRel();
	MQTTPubRel* mqMsg = static_cast<MQTTPubRel*>(msg);
	snMsg->setMsgId(mqMsg->getMessageId());
	printf(BLUE_FORMAT1, currentDateTime(), "PUBREL", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));
	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Downstream MQTTPubComp
 -------------------------------------------------------*/
void GatewayControlTask::handlePubComp(Event* ev, ClientNode* clnode, MQTTMessage* msg){
	MQTTSnPubComp* snMsg = new MQTTSnPubComp();
	MQTTPubComp* mqMsg = static_cast<MQTTPubComp*>(msg);
	snMsg->setMsgId(mqMsg->getMessageId());
	printf(BLUE_FORMAT1, currentDateTime(), "PUBCOMP", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));
	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}


/*-------------------------------------------------------
                Downstream MQTTPingResp
 -------------------------------------------------------*/
void GatewayControlTask::handlePingresp(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTSnPingResp* snMsg = new MQTTSnPingResp();
	//MQTTPingResp* mqMsg = static_cast<MQTTPingResp*>(msg);
	printf(FORMAT1, currentDateTime(), "PINGRESP", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Downstream MQTTSubAck
 -------------------------------------------------------*/
void GatewayControlTask::handleSuback(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTSubAck* mqMsg = static_cast<MQTTSubAck*>(msg);
	MQTTSnSubAck* snMsg = clnode->getWaitedSubAck();
	if(snMsg){
		if(snMsg->getMsgId() == mqMsg->getMessageId()){
			clnode->setWaitedSubAck(0);
			if(mqMsg->getGrantedQos() == 0x80){
				snMsg->setReturnCode(MQTTSN_RC_REJECTED_INVALID_TOPIC_ID);
			}else{
				snMsg->setReturnCode(MQTTSN_RC_ACCEPTED);
				snMsg->setQos(mqMsg->getGrantedQos());
			}
			printf(FORMAT1, currentDateTime(), "SUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

			clnode->setClientSendMessage(snMsg);

			Event* ev1 = new Event();
			ev1->setClientSendEvent(clnode);
			_res->getClientSendQue()->post(ev1);
		}
	}
}

/*-------------------------------------------------------
                Downstream MQTTUnsubAck
 -------------------------------------------------------*/
void GatewayControlTask::handleUnsuback(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTUnsubAck* mqMsg = static_cast<MQTTUnsubAck*>(msg);
	MQTTSnUnsubAck* snMsg = new MQTTSnUnsubAck();

	snMsg->setMsgId(mqMsg->getMessageId());
	printf(FORMAT1, currentDateTime(), "UNSUBACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}


/*-------------------------------------------------------
                Downstream MQTTConnAck
 -------------------------------------------------------*/
void GatewayControlTask::handleConnack(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTConnAck* mqMsg = static_cast<MQTTConnAck*>(msg);
	MQTTSnConnack* snMsg = new MQTTSnConnack();

	if(mqMsg->getReturnCd() == 0){
		snMsg->setReturnCode(MQTTSN_RC_ACCEPTED);
	}else if(mqMsg->getReturnCd() == MQTT_RC_REFUSED_PROTOCOL_VERSION){
		snMsg->setReturnCode(MQTTSN_RC_REJECTED_NOT_SUPPORTED);
		_protocol = (_protocol == MQTT_PROTOCOL_VER4) ? MQTT_PROTOCOL_VER3 : MQTT_PROTOCOL_VER4;
	}else if(mqMsg->getReturnCd() == MQTT_RC_REFUSED_SERVER_UNAVAILABLE){
		snMsg->setReturnCode(MQTTSN_RC_REJECTED_CONGESTION);
	}else{
		snMsg->setReturnCode(MQTTSN_RC_REJECTED_INVALID_TOPIC_ID);
	}
	printf(CYAN_FORMAT1, currentDateTime(), "CONNACK", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

	clnode->setClientSendMessage(snMsg);

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Downstream MQTTDisconnect
 -------------------------------------------------------*/
void GatewayControlTask::handleDisconnect(Event* ev, ClientNode* clnode, MQTTMessage* msg){
	MQTTSnDisconnect* snMsg = new MQTTSnDisconnect();
	//MQTTDisconnect* mqMsg = static_cast<MQTTDisconnect*>(msg);
	clnode->setClientSendMessage(snMsg);
	printf(FORMAT1, currentDateTime(), "DISCONNECT", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);
}

/*-------------------------------------------------------
                Downstream MQTTPublish
 -------------------------------------------------------*/
void GatewayControlTask::handlePublish(Event* ev, ClientNode* clnode, MQTTMessage* msg){

	MQTTPublish* mqMsg = static_cast<MQTTPublish*>(msg);
	MQTTSnPublish* snMsg = new MQTTSnPublish();

	string* tp = mqMsg->getTopic();
	uint16_t tpId;

	if(tp->size() == 2){
		tpId = getUint16((uint8_t*)tp);
		snMsg->setFlags(MQTTSN_TOPIC_TYPE_SHORT);
	}else{
		tpId = clnode->getTopics()->getTopicId(tp);
		snMsg->setFlags(MQTTSN_TOPIC_TYPE_NORMAL);
	}
	if(tpId == 0){
		/* ----- may be a publish message response of subscribed with '#' or '+' -----*/
		tpId = clnode->getTopics()->createTopic(tp);

		if(tpId > 0){
			MQTTSnRegister* regMsg = new MQTTSnRegister();
			regMsg->setTopicId(tpId);
			regMsg->setTopicName(tp);
			printf(FORMAT2, currentDateTime(), "REGISTER", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(regMsg));

			clnode->setClientSendMessage(regMsg);
			Event* evrg = new Event();
			evrg->setClientSendEvent(clnode);
			_res->getClientSendQue()->post(evrg);   // Send Register first.
		}else{
			printf("GatewayControlTask Can't create Topic   %s\n", tp->c_str());
			return;
		}
	}

	snMsg->setTopicId(tpId);
	snMsg->setMsgId(mqMsg->getMessageId());
	snMsg->setData(mqMsg->getPayload(),mqMsg->getPayloadLength());
	snMsg->setQos(mqMsg->getQos());

	if(mqMsg->isDup()){
		snMsg->setDup();
	}

	if(mqMsg->isRetain()){
		snMsg->setDup();
	}
	clnode->setClientSendMessage(snMsg);
	printf(GREEN_FORMAT1, currentDateTime(), "PUBLISH", RIGHTARROW, clnode->getNodeId()->c_str(), msgPrint(snMsg));

	Event* ev1 = new Event();
	ev1->setClientSendEvent(clnode);
	_res->getClientSendQue()->post(ev1);

}

char*  GatewayControlTask::msgPrint(MQTTSnMessage* msg){

	char* buf = _printBuf;
	for(int i = 0; i < msg->getBodyLength(); i++){
		sprintf(buf," %02X", *(msg->getBodyPtr() + i));
		buf += 3;
	}
	*buf = 0;
	return _printBuf;
}

char*  GatewayControlTask::msgPrint(MQTTMessage* msg){
	uint8_t sbuf[512];
	char* buf = _printBuf;
	msg->serialize(sbuf);

	for(int i = 0; i < msg->getRemainLength() + msg->getRemainLengthSize(); i++){
		//sprintf(buf, " %02X", *( sbuf + msg->getRemainLengthSize() + i));
		sprintf(buf, " %02X", *( sbuf + i));
		buf += 3;
	}
	*buf = 0;
	return _printBuf;
}


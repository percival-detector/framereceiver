/*
 * FileWriterController.cpp
 *
 *  Created on: 27 May 2016
 *      Author: gnx91527
 */

#include <FileWriterController.h>

#include <stdio.h>

namespace filewriter
{
  const std::string FileWriterController::CONFIG_SHUTDOWN          = "shutdown";

  const std::string FileWriterController::CONFIG_FR_SHARED_MEMORY  = "fr_shared_mem";
  const std::string FileWriterController::CONFIG_FR_RELEASE        = "fr_release_cnxn";
  const std::string FileWriterController::CONFIG_FR_READY          = "fr_ready_cnxn";
  const std::string FileWriterController::CONFIG_FR_SETUP          = "fr_setup";

  const std::string FileWriterController::CONFIG_CTRL_ENDPOINT     = "ctrl_endpoint";

  const std::string FileWriterController::CONFIG_PLUGIN            = "plugin";
  const std::string FileWriterController::CONFIG_PLUGIN_LIST       = "list";
  const std::string FileWriterController::CONFIG_PLUGIN_LOAD       = "load";
  const std::string FileWriterController::CONFIG_PLUGIN_CONNECT    = "connect";
  const std::string FileWriterController::CONFIG_PLUGIN_DISCONNECT = "disconnect";
  const std::string FileWriterController::CONFIG_PLUGIN_NAME       = "name";
  const std::string FileWriterController::CONFIG_PLUGIN_INDEX      = "index";
  const std::string FileWriterController::CONFIG_PLUGIN_LIBRARY    = "library";
  const std::string FileWriterController::CONFIG_PLUGIN_CONNECTION = "connection";

  FileWriterController::FileWriterController() :
    logger_(log4cxx::Logger::getLogger("FileWriterController")),
    runThread_(true),
    threadRunning_(false),
    threadInitError_(false),
    ctrlThread_(boost::bind(&FileWriterController::runIpcService, this)),
    ctrlChannel_(ZMQ_PAIR)
  {
    LOG4CXX_DEBUG(logger_, "Constructing FileWriterController");

    // Wait for the thread service to initialise and be running properly, so that
    // this constructor only returns once the object is fully initialised (RAII).
    // Monitor the thread error flag and throw an exception if initialisation fails
    while (!threadRunning_)
    {
        if (threadInitError_) {
            ctrlThread_.join();
            throw std::runtime_error(threadInitMsg_);
            break;
        }
    }

  }

  FileWriterController::~FileWriterController()
  {
    // TODO Auto-generated destructor stub
  }

  void FileWriterController::handleCtrlChannel()
  {
    // Receive a message from the main thread channel
    std::string ctrlMsgEncoded = ctrlChannel_.recv();

    LOG4CXX_DEBUG(logger_, "Control thread called with message: " << ctrlMsgEncoded);

    // Parse and handle the message
    try {
      FrameReceiver::IpcMessage ctrlMsg(ctrlMsgEncoded.c_str());
      FrameReceiver::IpcMessage replyMsg(FrameReceiver::IpcMessage::MsgTypeAck, FrameReceiver::IpcMessage::MsgValCmdConfigure);

      if ((ctrlMsg.get_msg_type() == FrameReceiver::IpcMessage::MsgTypeCmd) &&
          (ctrlMsg.get_msg_val()  == FrameReceiver::IpcMessage::MsgValCmdConfigure)){
        this->configure(ctrlMsg, replyMsg);
        LOG4CXX_DEBUG(logger_, "Control thread reply message: " << replyMsg.encode());
        ctrlChannel_.send(replyMsg.encode());
      } else {
        LOG4CXX_ERROR(logger_, "Control thread got unexpected message: " << ctrlMsgEncoded);
      }
    }
    catch (FrameReceiver::IpcMessageException& e)
    {
        LOG4CXX_ERROR(logger_, "Error decoding control channel request: " << e.what());
    }
    catch (std::runtime_error& e)
    {
        LOG4CXX_ERROR(logger_, "Bad control message: " << e.what());
        FrameReceiver::IpcMessage replyMsg(FrameReceiver::IpcMessage::MsgTypeNack, FrameReceiver::IpcMessage::MsgValCmdConfigure);
        replyMsg.set_param<std::string>("error", std::string(e.what()));
        ctrlChannel_.send(replyMsg.encode());
    }
  }

  void FileWriterController::configure(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
  {
    LOG4CXX_DEBUG(logger_, "Configuration submitted: " << config.encode());

    // Check if we are being asked to shutdown
    if (config.has_param(FileWriterController::CONFIG_SHUTDOWN)){
      exitCondition_.notify_all();
    }

    if (config.has_param(FileWriterController::CONFIG_CTRL_ENDPOINT)){
      std::string endpoint = config.get_param<std::string>(FileWriterController::CONFIG_CTRL_ENDPOINT);
      this->setupControlInterface(endpoint);
    }

    if (config.has_param(FileWriterController::CONFIG_PLUGIN)){
      FrameReceiver::IpcMessage pluginConfig(config.get_param<const rapidjson::Value&>(FileWriterController::CONFIG_PLUGIN));
      this->configurePlugin(pluginConfig, reply);
    }

    // Check if we are being passed the shared memory configuration
    if (config.has_param(FileWriterController::CONFIG_FR_SETUP)){
      FrameReceiver::IpcMessage frConfig(config.get_param<const rapidjson::Value&>(FileWriterController::CONFIG_FR_SETUP));
      if (frConfig.has_param(FileWriterController::CONFIG_FR_SHARED_MEMORY) &&
          frConfig.has_param(FileWriterController::CONFIG_FR_RELEASE) &&
          frConfig.has_param(FileWriterController::CONFIG_FR_READY)){
        std::string shMemName = frConfig.get_param<std::string>(FileWriterController::CONFIG_FR_SHARED_MEMORY);
        std::string pubString = frConfig.get_param<std::string>(FileWriterController::CONFIG_FR_RELEASE);
        std::string subString = frConfig.get_param<std::string>(FileWriterController::CONFIG_FR_READY);
        this->setupFrameReceiverInterface(shMemName, pubString, subString);
      }
    }

    // Loop over plugins, checking for configuration messages
    std::map<std::string, boost::shared_ptr<FileWriterPlugin> >::iterator iter;
    for (iter = plugins_.begin(); iter != plugins_.end(); ++iter){
      if (config.has_param(iter->first)){
        FrameReceiver::IpcMessage subConfig(config.get_param<const rapidjson::Value&>(iter->first));
        iter->second->configure(subConfig);
      }
    }
  }

  void FileWriterController::configurePlugin(FrameReceiver::IpcMessage& config, FrameReceiver::IpcMessage& reply)
  {
    if (config.has_param(FileWriterController::CONFIG_PLUGIN_LIST)){
      // We have been asked to list the loaded plugins
      rapidjson::Document jsonDoc;
      jsonDoc.SetObject();
      rapidjson::Document::AllocatorType& allocator = jsonDoc.GetAllocator();
      rapidjson::Value list(rapidjson::kArrayType);
      rapidjson::Value val;
      val.SetObject();
      std::map<std::string, boost::shared_ptr<FileWriterPlugin> >::iterator iter;
      for (iter = plugins_.begin(); iter != plugins_.end(); ++iter){
        val.SetObject();
        val.SetString(iter->first.c_str(), allocator);
        list.PushBack(val, allocator);
      }
      reply.set_param("plugins", list);
    }

    // Check if we are being asked to load a plugin
    if (config.has_param(FileWriterController::CONFIG_PLUGIN_LOAD)){
      FrameReceiver::IpcMessage pluginConfig(config.get_param<const rapidjson::Value&>(FileWriterController::CONFIG_PLUGIN_LOAD));
      if (pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_NAME) &&
          pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_INDEX) &&
          pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_LIBRARY)){
        std::string index = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_INDEX);
        std::string name = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_NAME);
        std::string library = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_LIBRARY);
        this->loadPlugin(index, name, library);
      }
    }

    // Check if we are being asked to connect a plugin
    if (config.has_param(FileWriterController::CONFIG_PLUGIN_CONNECT)){
      FrameReceiver::IpcMessage pluginConfig(config.get_param<const rapidjson::Value&>(FileWriterController::CONFIG_PLUGIN_CONNECT));
      if (pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_CONNECTION) &&
          pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_INDEX)){
        std::string index = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_INDEX);
        std::string cnxn = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_CONNECTION);
        this->connectPlugin(index, cnxn);
      }
    }

    // Check if we are being asked to disconnect a plugin
    if (config.has_param(FileWriterController::CONFIG_PLUGIN_DISCONNECT)){
      FrameReceiver::IpcMessage pluginConfig(config.get_param<const rapidjson::Value&>(FileWriterController::CONFIG_PLUGIN_DISCONNECT));
      if (pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_CONNECTION) &&
          pluginConfig.has_param(FileWriterController::CONFIG_PLUGIN_INDEX)){
        std::string index = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_INDEX);
        std::string cnxn = pluginConfig.get_param<std::string>(FileWriterController::CONFIG_PLUGIN_CONNECTION);
        this->disconnectPlugin(index, cnxn);
      }
    }
  }

  void FileWriterController::loadPlugin(const std::string& index, const std::string& name, const std::string& library)
  {
    // Verify a plugin of the same name doesn't already exist
    if (plugins_.count(index) == 0){
      // Dynamically class load the plugin
      // Add the plugin to the map, indexed by the name
      boost::shared_ptr<FileWriterPlugin> plugin = ClassLoader<FileWriterPlugin>::load_class(name, library);
      plugin->setName(index);
      plugins_[index] = plugin;
      // Start the plugin worker thread
      plugin->start();
    } else {
      LOG4CXX_ERROR(logger_, "Cannot load plugin with index = " << index << ", already loaded");
      std::stringstream is;
      is << "Cannot load plugin with index = " << index << ", already loaded";
      throw std::runtime_error(is.str().c_str());
    }
  }

  void FileWriterController::connectPlugin(const std::string& index, const std::string& connectTo)
  {
    // Check that the plugin is loaded
    if (plugins_.count(index) > 0){
      // Check for the shared memory connection
      if (connectTo == "frame_receiver"){
        sharedMemController_->registerCallback(index, plugins_[index]);
      } else {
        if (plugins_.count(connectTo) > 0){
          plugins_[connectTo]->registerCallback(index, plugins_[index]);
        }
      }
    } else {
      LOG4CXX_ERROR(logger_, "Cannot connect plugin with index = " << index << ", plugin isn't loaded");
      std::stringstream is;
      is << "Cannot connect plugin with index = " << index << ", plugin isn't loaded";
      throw std::runtime_error(is.str().c_str());
    }
  }

  void FileWriterController::disconnectPlugin(const std::string& index, const std::string& disconnectFrom)
  {
    // Check that the plugin is loaded
    if (plugins_.count(index) > 0){
      // Check for the shared memory connection
      if (disconnectFrom == "frame_receiver"){
        sharedMemController_->removeCallback(index);
      } else {
        if (plugins_.count(disconnectFrom) > 0){
          plugins_[disconnectFrom]->removeCallback(index);
        }
      }
    } else {
      LOG4CXX_ERROR(logger_, "Cannot disconnect plugin with index = " << index << ", plugin isn't loaded");
      std::stringstream is;
      is << "Cannot disconnect plugin with index = " << index << ", plugin isn't loaded";
      throw std::runtime_error(is.str().c_str());
    }
  }

  void FileWriterController::waitForShutdown()
  {
    boost::unique_lock<boost::mutex> lock(exitMutex_);
    exitCondition_.wait(lock);
  }

  void FileWriterController::setupFrameReceiverInterface(const std::string& sharedMemName,
                                                         const std::string& frPublisherString,
                                                         const std::string& frSubscriberString)
  {
    LOG4CXX_DEBUG(logger_, "Shared Memory Config: Name=" << sharedMemName <<
                  " Publisher=" << frPublisherString << " Subscriber=" << frSubscriberString);

    // Release current shared memory parser if one exists
    if (sharedMemParser_){
      sharedMemParser_.reset();
    }
    // Create the new shared memory parser
    sharedMemParser_ = boost::shared_ptr<SharedMemoryParser>(new SharedMemoryParser(sharedMemName));

    // Release the current shared memory controller if one exists
    if (sharedMemController_){
      sharedMemController_.reset();
    }
    // Create the new shared memory controller and give it the parser and publisher
    sharedMemController_ = boost::shared_ptr<SharedMemoryController>(new SharedMemoryController(reactor_, frSubscriberString, frPublisherString));
    sharedMemController_->setSharedMemoryParser(sharedMemParser_);
  }

  void FileWriterController::setupControlInterface(const std::string& ctrlEndpointString)
  {
    try {
      LOG4CXX_DEBUG(logger_, "Connecting control channel to endpoint: " << ctrlEndpointString);
      ctrlChannel_.bind(ctrlEndpointString.c_str());
    }
    catch (zmq::error_t& e) {
      //std::stringstream ss;
      //ss << "RX channel connect to endpoint " << config_.rx_channel_endpoint_ << " failed: " << e.what();
      // TODO: What to do here, I think throw it up
      throw std::runtime_error(e.what());
    }

    // Add the control channel to the reactor
    reactor_->register_channel(ctrlChannel_, boost::bind(&FileWriterController::handleCtrlChannel, this));
  }

  void FileWriterController::runIpcService(void)
  {
    LOG4CXX_DEBUG(logger_, "Running IPC thread service");

    // Create the reactor
    reactor_ = boost::shared_ptr<FrameReceiver::IpcReactor>(new FrameReceiver::IpcReactor());

    // Add the tick timer to the reactor
    int tick_timer_id = reactor_->register_timer(1000, 0, boost::bind(&FileWriterController::tickTimer, this));

    // Add the buffer monitor timer to the reactor
    //int buffer_monitor_timer_id = reactor_.register_timer(3000, 0, boost::bind(&FrameReceiverRxThread::buffer_monitor_timer, this));

    // Register the frame release callback with the decoder
    //frame_decoder_->register_frame_ready_callback(boost::bind(&FrameReceiverRxThread::frame_ready, this, _1, _2));

    // Set thread state to running, allows constructor to return
    threadRunning_ = true;

    // Run the reactor event loop
    reactor_->run();

    // Cleanup - remove channels, sockets and timers from the reactor and close the receive socket
    LOG4CXX_DEBUG(logger_, "Terminating IPC thread service");
  }

  void FileWriterController::tickTimer(void)
  {
    //LOG4CXX_DEBUG(logger_, "IPC thread tick timer fired");
    if (!runThread_)
    {
      LOG4CXX_DEBUG(logger_, "IPC thread terminate detected in timer");
      reactor_->stop();
    }
  }

} /* namespace filewriter */
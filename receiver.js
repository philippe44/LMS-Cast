      const context = cast.framework.CastReceiverContext.getInstance();
      const playerManager = context.getPlayerManager();

      // helper : query elements even deeply within shadow doms
      function querySelectorDeep(selector, root = document) {
        let currentRoot = root;
        let partials = selector.split('::shadow');
        let elems = currentRoot.querySelectorAll(partials[0]);
        for (let i = 1; i < partials.length; i++) {
          let partial = partials[i];
          let elemsInside = [];
          for (let j = 0; j < elems.length; j++) {
            let shadow = elems[j].shadowRoot;
            if (shadow) {
              const matchesInShadow = shadow.querySelectorAll(partial);
              elemsInside = elemsInside.concat([... matchesInShadow]);
            }
          }
          elems = elemsInside;
        }
        return elems;
      }

      // update application name field
      function updateAppName(str){
        try {
          // retrieve the right div - it's inside two shadowRoot(s) so we need a special selector
          const mdivTab = querySelectorDeep('cast-media-player::shadow tv-overlay::shadow div.playback-logo')
          if (mdivTab.length > 0){
            const mdiv = mdivTab[0]
            // update text
            mdiv.innerHTML = str
            // update color
            mdiv.style.color = 'rgba(255,255,255,0.4)'
          }
        } catch(err){
          console.warn("updateAppName error", err)
        }
      }

      // update metadata
      function updateMetadata(metaData){
        try {
          // retrieve current media information
          const mediaInformation = playerManager.getMediaInformation()
          // update its metadata
          mediaInformation.metadata = metaData
          // update current media information
          playerManager.setMediaInformation(mediaInformation)
        } catch(err){
          console.warn("updateMetadata error", err)
        }
      }
        
      // Sender GET_STATUS request
      playerManager.setMessageInterceptor(
        cast.framework.messages.MessageType.GET_STATUS, data => {
          console.log("MSG:GET_STATUS", data)
          // update Player metadata with data.customData.metadata if any
          if (data && data.customData && data.customData.metadata)
            updateMetadata(data.customData.metadata)
          return data
        }
      )
        
      // Sender LOAD request
      playerManager.setMessageInterceptor(
        cast.framework.messages.MessageType.LOAD, data => {
          console.log("MSG:LOAD", data)
          // update AppName with data.media.customData.deviceName if any
          if (data && data.media && data.media.customData && data.media.customData.deviceName)
            updateAppName(data.media.customData.deviceName)
          return data
        }
      )
        
      context.start();

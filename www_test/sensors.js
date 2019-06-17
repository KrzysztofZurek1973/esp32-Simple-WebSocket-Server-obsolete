var socket = new WebSocket("ws://esp32-ws.local:8080");

window.addEventListener("load", function(){ //when page loads
        console.log(timeConverter(Date.now()));
});

socket.onmessage = function (event) {
    var msg = JSON.parse(event.data);
	var ledTxt = document.getElementById("sensorTwo");
	var ledPict = document.getElementById("led_picture");
	
	//console.log(msg);
	
	switch(msg.type) {
    case "message":
        //var msgData = msg.data;
        switch(msg.data.sensor){
        case "counter":
		     var s41 = document.getElementById("s4_value");
		     s41.innerHTML = Number.parseInt(msg.data.value, 0);
		     var s45 = document.getElementById("s4_time");

			 s45.innerHTML = timeConverter(Date.now());
		     break;
        break;
        }
    }
};

//convert UNIX timestamp into time
function timeConverter(UNIX_timestamp){
  var a = new Date(UNIX_timestamp);
  var months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
  var year = a.getFullYear();
  var month = months[a.getMonth()];
  var date = '0' + a.getDate();
  var hour = '0' + a.getHours();
  var min = '0' + a.getMinutes();
  var sec = '0' + a.getSeconds();
  var time = date.substr(-2) + ' ' + month + ' ' + year + ' ' +
		hour.substr(-2) + ':' + min.substr(-2) + ':' + sec.substr(-2);
  return time;
}

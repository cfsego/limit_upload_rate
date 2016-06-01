#README#
ngx_limit_upload_module is a tengine module, which allows you to limit client-upload rate
when they are sending request bodies to you. the usage is just the same as limit_rate functionality
of stardard nginx. If you want to use it with nginx, see Chapter "USE WITH NGINX" for details.

##Directive##

###limit_upload_rate rate###

    Syntax:    limit_upload_rate rate;
    Default:   limit_upload_rate 0;
    Context:   http, server, location, if in location

Rate limits the transmission of request body from a client. The rate is specified in bytes per second.
The value 0 disables rate limiting. The limit is set per request, so if a client simultaneously opens
two connections, an overall rate will be twice as much as the specified limit.

Rate limit can also be set in the $limit_upload_rate variable. It may be useful in cases where rate
should be limited depending on a certain condition:

    server {

        if ($slow) {
            set $limit_rate 4k;
        }

        ...
    }

###limit_upload_rate_after###

    Syntax:     limit_upload_rate_after size;
    Default:    limit_upload_rate_after 0;
    Context:    http, server, location, if in location

Sets the initial amount after which the further transmission of a request body from a client will be rate limited.

##USE WITH TENGINE##
the module needs the latest input body filter in tengine, otherwise the functionality of this module is totally
unavailable. the update can be found at https://github.com/taobao/tengine/pull/136

    ./configure --add-module=limit_upload_rate
    make

##USE WITH NGINX##
the module needs the latest input body filter in tengine, so you have to patch nginx if you want to use this module.
the patch file is included with the filename "for-nginx.patch" and is verified under nginx 1.2.5.
for-nginx-1.4.4.patch is base on nginx 1.4.4 and can be used with nginx 1.5.X

    patch -p1 < for-nginx-1.4.4.patch
    ./configure --add-module=limit_upload_rate
    make

##CHANGES##
1.0.1     2012-12-24    bugfix: conflict with "client_body_timeout"
1.0.0     2012-12-07    initial version

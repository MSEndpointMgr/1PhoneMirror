targetScope = 'subscription'

@minLength(1)
@maxLength(64)
@description('Name of the the environment. Used as a prefix for naming resources.')
param environmentName string

@minLength(1)
@description('Primary location for all resources.')
param location string

@description('Id of the principal to assign Storage Blob Data Owner + Monitoring Metrics Publisher (you, when running locally). Leave empty in CI.')
param principalId string = ''

var abbrs = loadJsonContent('./abbreviations.json')
var resourceToken = toLower(uniqueString(subscription().id, environmentName, location))
var tags = {
  'azd-env-name': environmentName
  project: '1PhoneMirror-Telemetry'
}

// RG name pattern: '{environmentName}-rg' so that environmentName='1phonemirror-prod'
// produces the exact group name '1phonemirror-prod-rg'.
resource rg 'Microsoft.Resources/resourceGroups@2024-03-01' = {
  name: '${environmentName}-rg'
  location: location
  tags: tags
}

module resources './resources.bicep' = {
  name: 'resources'
  scope: rg
  params: {
    location: location
    resourceToken: resourceToken
    tags: tags
    principalId: principalId
  }
}

output AZURE_LOCATION string = location
output AZURE_TENANT_ID string = tenant().tenantId
output RESOURCE_GROUP_NAME string = rg.name
output FUNCTION_APP_NAME string = resources.outputs.functionAppName
output FUNCTION_APP_URL string = resources.outputs.functionAppUrl
output APPLICATIONINSIGHTS_CONNECTION_STRING string = resources.outputs.appInsightsConnectionString
output APPLICATIONINSIGHTS_NAME string = resources.outputs.appInsightsName
output LOG_ANALYTICS_WORKSPACE_NAME string = resources.outputs.logAnalyticsWorkspaceName
